"""End-to-end check of the hidden self-test mode (01 §17, 05 §5, DECISIONS D-30).

Runs the real WizzairHelper.exe as

    WizzairHelper.exe --autotest <work>/out --data-root <work>/data
                      --replay <replay dir> --fixed-now 2026-11-02T10:00:00Z --diagnostics
                      <scenario flags>

on an isolated profile and a replayed network, then checks summary.json, the
test-only diagnostics.json (40 ms timer gaps, cancel transitions, final grids,
layout and display) and the screenshots. The timer gaps and UI dispatches are
checked by the UI thread's own CPU time always and by their wall-clock length
unless other processes held the CPUs during them (DECISIONS D-50), so a busy
machine does not fail a run. Run with `python -I` (no site or user
paths). `--scale 1.5` / `--scale 2` run the window at 150 % / 200 % through
QT_SCALE_FACTOR (01 §15, 05 §5).

The replay directory is tests/replay/<scenario>, except for the heavy cached
replays (heavy_cached, cancel_cached), whose timetables are generated into
<work>/replay from the frozen live route map: one fare per day on every route
from Budapest over 105 days (42-day chunks).

summary.json carries no booking URLs (01 §17.3, 01-digest C-07); the links the grids
open are asserted by the in-process GUI tests (ControllerFlows.*).

The crash_* scenarios start the exe with a command line that fails at start-up (01 §3.2,
D-30): they wait for the crash box, check that the process is per-monitor-v2 DPI aware
by then and the box is not DPI-unaware, i.e. not bitmap-stretched (01 §3, 06 §4), and
that it shows the error, press OK, and check the errors.log record in the --data-root
profile (and that the real %LOCALAPPDATA%\\WizzairHelper\\errors.log did not grow).
"""
import argparse
import ctypes
import datetime
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from ctypes import wintypes

FIXED_NOW = '2026-11-02T10:00:00Z'
STARTED = ('[10:00:00] WizzairHelper started. Fares: wizzair.com (unofficial website API). Geocoding: Photon / '
           'Nominatim, data © OpenStreetMap contributors. Exchange rates: Frankfurter / currency-api.')
ROUTE_MAP_LOG = '[10:00:00] Route map: {} airports, API https://be.wizzair.com/29.23.1/Api.'
KEY_ORDER = ['startedAt', 'apiUrl', 'airports', 'outbound', 'return', 'roundTrips', 'afterCurrencySwitch',
             'exception', 'progressScreenshots', 'finalStatus', 'requestsToWizz', 'log']
DIAGNOSTICS_KEYS = ['timer', 'uiEvents', 'cancel', 'searches', 'grids', 'display', 'layout', 'environment']
CANCELLING = 'Cancelling… (finishing the current request)'
PRESSES = ['Esc', 'Cancel button', 'Esc', 'Cancel handler']


class Checker:
    def __init__(self, summary, diagnostics, out_dir):
        self.summary = summary
        self.diagnostics = diagnostics
        self.out_dir = out_dir
        self.failures = []

    def expect(self, condition, message):
        if not condition:
            self.failures.append(message)

    def equal(self, path, expected):
        actual = self.get(path)
        self.expect(actual == expected, '%s: expected %r, got %r' % (path, expected, actual))
        return actual

    def get(self, path, root=None):
        value = self.summary if root is None else root
        for part in path.split('.'):
            if isinstance(value, dict):
                value = value.get(part)
            elif isinstance(value, list) and part.isdigit() and int(part) < len(value):
                value = value[int(part)]
            else:
                return None
        return value

    def diag(self, path):
        return self.get(path, self.diagnostics)

    def diag_equal(self, path, expected):
        actual = self.diag(path)
        self.expect(actual == expected, 'diagnostics %s: expected %r, got %r' % (path, expected, actual))
        return actual

    def keys(self, expected):
        self.equal_list('top-level keys', list(self.summary.keys()), expected)
        self.expect([k for k in self.summary if k in KEY_ORDER] == [k for k in KEY_ORDER if k in self.summary],
                    'top-level key order differs from 01 §17.2')

    def equal_list(self, what, actual, expected):
        self.expect(actual == expected, '%s: expected %r, got %r' % (what, expected, actual))

    def log_has(self, line):
        lines = self.summary.get('log', [])
        self.expect(any(entry == '[10:00:00] ' + line for entry in lines), 'log line missing: %r' % line)

    def leg(self, path, flights, rows, planned, completed, origins, destinations, view, current, fx=True,
            cancelled=False):
        self.equal(path + '.flights', flights)
        self.equal(path + '.gridRows', rows)
        self.equal(path + '.requestsPlanned', planned)
        self.equal(path + '.requestsCompleted', completed)
        self.equal(path + '.cancelled', cancelled)
        self.equal(path + '.abortReason', None)
        self.equal(path + '.elapsedSeconds', 0)  # FixedClock: the monotonic time does not move
        self.equal(path + '.originAirports', origins)
        self.equal(path + '.destinationAirports', destinations)
        if fx:
            self.equal(path + '.fx', 'Frankfurter 2026-11-02')
        self.equal(path + '.view', view)
        self.equal(path + '.currentRow', current)
        top = self.get(path + '.topRows') or []
        self.expect(len(top) == min(rows, 8), '%s.topRows: expected %d rows, got %d' % (path, min(rows, 8), len(top)))

    def screenshots(self, names):
        for name in names:
            path = os.path.join(self.out_dir, name)
            if not os.path.isfile(path):
                self.failures.append('screenshot missing: ' + name)
                continue
            with open(path, 'rb') as f:
                head = f.read(24)
            if head[:8] != b'\x89PNG\r\n\x1a\n' or head[12:16] != b'IHDR':
                self.failures.append('not a PNG: ' + name)
                continue
            width, height = struct.unpack('>II', head[16:24])
            # The whole window: at least L(980) x L(600) minus the invisible borders.
            self.expect(width >= 960 and height >= 580, '%s: %dx%d is smaller than the window' % (name, width, height))
        for line in self.summary.get('log', []):
            self.expect(' screenshot ' not in line or ' failed: ' not in line, 'screenshot failure logged: ' + line)

    def progress(self, at_least, at_most=4, prefix='Fetching fares'):
        shots = self.summary.get('progressScreenshots', [])
        self.expect(at_least <= len(shots) <= at_most,
                    'progressScreenshots: expected %d..%d entries, got %d' % (at_least, at_most, len(shots)))
        for status in shots:
            self.expect(status.startswith(prefix), 'progress status: %r' % status)
        self.screenshots(['progress-%d.png' % (i + 1) for i in range(len(shots))])
        extra = os.path.join(self.out_dir, 'progress-%d.png' % (len(shots) + 1))
        self.expect(not os.path.exists(extra), 'more progress screenshots than progressScreenshots entries')

    def common(self, airports):
        self.equal('startedAt', '2026-11-02T10:00:00')
        self.equal('apiUrl', 'https://be.wizzair.com/29.23.1/Api')
        self.equal('airports', airports)
        self.expect('exception' not in self.summary, 'exception: %r' % self.summary.get('exception'))
        log = self.summary.get('log', [])
        self.expect(len(log) >= 2 and log[0] == STARTED, 'log[0] is not the start-up line: %r' % (log[:1],))
        self.expect(len(log) >= 2 and log[1] == ROUTE_MAP_LOG.format(airports), 'log[1]: %r' % (log[1:2],))
        self.expect(all(line.startswith('[10:00:00] ') for line in log), 'log lines need the fixed-clock prefix')

    # --- diagnostics.json (D-30 --diagnostics) ----------------------------------------------

    def diagnostics_common(self, max_gap_ms, scale):
        d = self.diagnostics
        self.expect(list(d.keys()) == DIAGNOSTICS_KEYS, 'diagnostics keys: %r' % list(d.keys()))
        # 05 §5 / R-03: the 40 ms progress timer never pauses for 100 ms or more. On a busy machine a
        # gap also grows while the OS runs other processes, so (DECISIONS D-50) the UI thread's own
        # CPU time in a gap is checked always, and the gap itself whenever other processes did not
        # hold the CPUs during it (idle CPUs or the app's own threads: then the whole gap is the app's).
        cpu = self.diag('timer.longestGapCpuMs')
        self.expect(cpu is not None and cpu < max_gap_ms,
                    'timer: the UI thread worked %r ms in one gap (not below %r ms): %r' %
                    (cpu, max_gap_ms, (self.diag('timer.longestGaps') or [])[:2]))
        own = self.diag('timer.longestOwnGapMs')
        self.expect(own is not None and own < max_gap_ms,
                    'timer: a gap of %r ms that other processes did not cause is not below %r ms (longest of all '
                    '%r ms, at %r): %r' % (own, max_gap_ms, self.diag('timer.longestGapMs'),
                                           self.diag('timer.longestGapStatus'),
                                           (self.diag('timer.longestGaps') or [])[:2]))
        self.expect((self.diag('timer.runs') or 0) >= 1, 'timer: no busy period was measured')
        # R-03: no uninterrupted UI-thread work of 100 ms or more while busy (UI-loop watchdog), the
        # same two ways.
        busy_cpu = self.diag('uiEvents.longestBusyCpuMs')
        self.expect(busy_cpu is not None and busy_cpu < max_gap_ms,
                    'uiEvents: a dispatch worked %r ms while busy (not below %r ms): %r' %
                    (busy_cpu, max_gap_ms, (self.diag('uiEvents.longest') or [])[:2]))
        busy_own = self.diag('uiEvents.longestBusyOwnMs')
        self.expect(busy_own is not None and busy_own < max_gap_ms,
                    'uiEvents: a dispatch ran %r ms while busy and other processes did not hold the CPUs (not '
                    'below %r ms): %r' % (busy_own, max_gap_ms, (self.diag('uiEvents.longest') or [])[:2]))
        # 01 §15: no clipping, overlap or hidden Cancel in any captured state.
        self.diag_equal('layout.problems', [])
        self.diag_equal('layout.cancelVisible', True)
        self.expect((self.diag('layout.statesChecked') or 0) >= 2, 'layout: fewer than 2 states checked')
        self.diag_equal('display.windowOnScreen', True)
        self.diag_equal('display.qtScaleFactor', scale)
        if scale is not None:
            self.expect(abs((self.diag('display.devicePixelRatio') or 0) - float(scale)) < 1e-9,
                        'display.devicePixelRatio: expected %s, got %r' % (scale, self.diag('display.devicePixelRatio')))
        # 05 §5: the final tables hold exactly the stored results (no late batch, no row of a
        # previous search, no duplicate).
        for name, grid in (d.get('grids') or {}).items():
            if grid.get('resultFlights') is None:
                continue
            self.expect(grid['rows'] == grid['resultFlights'],
                        'grids.%s: %d rows for %d result flights' % (name, grid['rows'], grid['resultFlights']))
            self.expect(grid['rowsNotInResult'] == 0, 'grids.%s: %d rows not in the result' %
                        (name, grid['rowsNotInResult']))
            self.expect(grid['duplicateRows'] == 0, 'grids.%s: %d duplicate rows' % (name, grid['duplicateRows']))

    def search(self, index, name, requests=None):
        self.diag_equal('searches.%d.name' % index, name)
        if requests is not None:
            self.diag_equal('searches.%d.wizzRequests' % index, requests)

    def single_cancel_transition(self, after_ms):
        """Esc, Cancel, Esc and the handler while busy: exactly one transition (D-11)."""
        self.diag_equal('cancel.afterMs', after_ms)
        self.diag_equal('cancel.presses', PRESSES)
        if self.diag('cancel.busyAtPress'):
            self.diag_equal('cancel.cancelRequests', 1)
            self.diag_equal('cancel.statusAfterPress', CANCELLING)
            self.diag_equal('cancel.cancelEnabledAfterPress', False)
        else:
            self.diag_equal('cancel.cancelRequests', 0)


def check_named_destination(c):
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'return', 'roundTrips', 'afterCurrencySwitch',
            'progressScreenshots', 'finalStatus', 'requestsToWizz', 'log'])
    c.common(11)
    c.leg('outbound', 12, 12, 3, 3, ['BUD 0km', 'BTS 173km', 'DEB 184km'], ['LGW 41km', 'LTN 44km'], 'AllFares',
          '2026-11-12 BTS->LTN 14.99')
    c.equal('outbound.warnings', [])
    c.equal('outbound.topRows.0', '2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total=14.99 local=14.99 EUR')
    c.equal('outbound.topRows.1', '2026-11-14 06:00 BUD  Budapest -> LTN  London Luton total=52.66 local=19,290 HUF')
    c.expect((c.get('outbound.progressUpdates') or 0) > 0, 'outbound.progressUpdates must count the reports')
    c.leg('return', 4, 4, 4, 4, ['LGW 41km', 'LTN 44km'], ['BUD 0km', 'BTS 173km', 'DEB 184km'], 'AllFares',
          '2026-11-17 LTN->BUD 21.54')
    c.equal('return.topRows.0', '2026-11-17 21:35 LTN  London Luton -> BUD  Budapest total=21.54 local=17.99 GBP')
    # Round trips (03 §11 legs, 1 adult, 2..10 nights).
    c.equal('roundTrips.count', 22)
    c.equal('roundTrips.top.0', '36.53 EUR: 2026-11-12 BTS->LTN + 2026-11-17 LTN->BUD (5 nights)')
    c.equal('roundTrips.top.1', '38.92 EUR: 2026-11-12 BTS->LTN + 2026-11-15 LTN->BTS (3 nights)')
    c.equal('roundTrips.summary', '22 combinations · cheapest 36.53 EUR: BTS→LTN Thu 12 Nov, back '
                                  'LTN→BUD Tue 17 Nov (5 nights)')
    # Currency switch: re-priced from the stored rates (03 §11 "Reprice": 14.99 EUR -> 5491 HUF).
    c.equal('afterCurrencySwitch.currency', 'HUF')
    c.leg('afterCurrencySwitch.outbound', 12, 12, 3, 3, ['BUD 0km', 'BTS 173km', 'DEB 184km'],
          ['LGW 41km', 'LTN 44km'], 'AllFares', '2026-11-12 BTS->LTN 5491')
    c.equal('afterCurrencySwitch.outbound.progressUpdates', 0)
    c.equal('afterCurrencySwitch.outbound.topRows.1',
            '2026-11-14 06:00 BUD  Budapest -> LTN  London Luton total=19290 local=19,290 HUF')
    c.equal('afterCurrencySwitch.roundTrips.count', 22)
    c.equal('afterCurrencySwitch.roundTrips.top.0', '13379 HUF: 2026-11-12 BTS->LTN + 2026-11-17 LTN->BUD (5 nights)')
    # Zero extra requests: homepage + map + every planned timetable request, nothing for the switch.
    planned = (c.get('outbound.requestsPlanned') or 0) + (c.get('return.requestsPlanned') or 0)
    c.equal('requestsToWizz', 2 + planned)
    c.equal('requestsToWizz', 9)
    c.equal('finalStatus', 'Return Done: 4 fares from 4 of 4 requests in 1 s.')
    c.log_has('Outbound: Budapest (BUD), Hungary (+200 km) → London, United Kingdom — Wizz Air airports: '
              'LGW, LTN (+30 km), 2026-11-10 … 2026-11-20, 1 adult(s), Regular, EUR')
    c.log_has('Outbound: 12 fares, 3/3 requests, 1 route(s) not served, 1 s, FX Frankfurter 2026-11-02')
    c.log_has('Return: 4 fares, 4/4 requests, 0 route(s) not served, 1 s, FX Frankfurter 2026-11-02')
    c.progress(1)
    c.screenshots(['1-outbound.png', '2-return.png', '3-roundtrips.png', '5-currency-switched.png', '4-log.png'])
    c.search(0, 'outbound', 3)
    c.search(1, 'return', 4)
    c.diag_equal('cancel', None)
    c.diag_equal('grids.outbound.rows', 12)
    c.diag_equal('grids.return.rows', 4)


def check_anywhere_return_from_top(c):
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'return', 'roundTrips', 'progressScreenshots',
            'finalStatus', 'requestsToWizz', 'log'])
    c.common(11)
    c.leg('outbound', 21, 2, 5, 5, ['BUD 0km'], [], 'CheapestPerDestination', '2026-12-01 BUD->NAP 31.64')
    c.equal('outbound.warnings', ['1 route(s) skipped because their next flight is after your dates: BUD → '
                                  'Alghero (Sardinia): next flight 07 Jun 2027.'])
    # --return-from-top: the right-click flow from the top fare's airport, radius min(To radius, 50).
    c.leg('return', 3, 3, 1, 1, ['NAP 0km'], ['BUD 0km'], 'AllFares', '2026-12-06 NAP->BUD 19.99')
    c.log_has('Return will start from Naples (NAP) (picked in the outbound results), 03 Dec – 11 Dec.')
    c.log_has('Return: Naples (NAP), Italy (+50 km) → Budapest (BUD), Hungary (+0 km), 2026-12-03 … '
              '2026-12-11, 1 adult(s), Regular, EUR')
    c.equal('roundTrips.count', 13)
    c.equal('roundTrips.top.0', '51.63 EUR: 2026-12-01 BUD->NAP + 2026-12-06 NAP->BUD (5 nights)')
    c.equal('requestsToWizz', 2 + 5 + 1)
    c.equal('finalStatus', 'Return Done: 3 fares from 1 of 1 requests in 1 s.')
    c.progress(1)
    c.screenshots(['1-outbound.png', '2-return.png', '3-roundtrips.png', '4-log.png'])
    c.search(0, 'outbound', 5)
    c.search(1, 'return', 1)
    c.diag_equal('grids.outbound.rows', 21)


def check_not_served(c):
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'progressScreenshots', 'finalStatus', 'requestsToWizz',
            'log'])
    c.common(11)
    c.leg('outbound', 0, 0, 1, 1, ['DEB 0km'], ['LGW 41km', 'LTN 44km'], 'AllFares', '(none)')
    c.equal('outbound.warnings', [])
    c.equal('finalStatus', 'Outbound Done: 0 fares from 1 of 1 requests in 1 s.')
    c.equal('requestsToWizz', 3)
    c.log_has('Outbound: 0 fares, 1/1 requests, 1 route(s) not served, 1 s, FX Frankfurter 2026-11-02')
    c.progress(0)
    c.screenshots(['1-outbound.png', '4-log.png'])
    c.search(0, 'outbound', 1)


def check_same_area(c):
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'progressScreenshots', 'finalStatus', 'requestsToWizz',
            'log'])
    c.common(11)
    c.equal('outbound', {'error': 'no result'})
    note = ('Start and destination are the same area (0 km apart). Choose a destination farther away, or leave '
            '“To” empty to search anywhere.')
    c.equal('finalStatus', note)
    c.equal('requestsToWizz', 2)
    c.log_has('  not searched: start and destination are the same area')
    c.log_has('  note: ' + note)
    c.progress(0)
    c.screenshots(['1-outbound.png', '4-log.png'])
    c.search(0, 'outbound', 0)
    c.diag_equal('grids.outbound.rows', 0)


def check_over_500(c):
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'progressScreenshots', 'finalStatus', 'requestsToWizz',
            'log'])
    c.common(189)
    c.equal('outbound', {'error': 'no result'})
    cap = ('This search would need 6794 requests to Wizz Air (about 148 min). That is more than the 500 this app '
           'allows, to stay polite to the site. Use a smaller radius, fewer dates, or a specific destination.')
    c.equal('finalStatus', cap)
    c.equal('requestsToWizz', 2)
    c.log_has('  404 route(s), 6794 request(s)')
    c.log_has(cap)
    c.progress(0)
    c.screenshots(['1-outbound.png', '4-log.png'])
    c.search(0, 'outbound', 0)


def check_cancel_waiting(c):
    """Cancel while the first timetable request waits for a server that does not answer (30 s)."""
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'progressScreenshots', 'finalStatus', 'requestsToWizz',
            'log'])
    c.common(11)
    c.leg('outbound', 0, 0, 3, 0, ['BUD 0km', 'BTS 173km', 'DEB 184km'], ['LGW 41km', 'LTN 44km'], 'AllFares',
          '(none)', cancelled=True)
    c.equal('outbound.warnings', [])
    c.equal('finalStatus', 'Outbound Cancelled: 0 fares from 0 of 3 requests in 1 s.')
    c.equal('requestsToWizz', 3)  # homepage, map, the one request that was waiting
    c.log_has('Outbound: 0 fares, 0/3 requests, 0 route(s) not served, 1 s, FX Frankfurter 2026-11-02')
    c.progress(0, 1)
    c.screenshots(['1-outbound.png', '4-log.png'])
    c.single_cancel_transition(1500)
    c.diag_equal('cancel.busyAtPress', True)
    c.expect((c.diag('cancel.statusBefore') or '').startswith('Fetching fares:  BUD'),
             'cancel.statusBefore: %r' % c.diag('cancel.statusBefore'))
    c.search(0, 'outbound', 1)
    # The 30 s wait was interrupted: the search ended soon after the presses.
    c.expect((c.diag('searches.0.durationMs') or 1e9) < 10000,
             'the cancelled search took %r ms' % c.diag('searches.0.durationMs'))


def check_cancel_retrying(c):
    """Cancel during the 5 s back-off after a 503 (04 §11, P-31 / P-32)."""
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'progressScreenshots', 'finalStatus', 'requestsToWizz',
            'log'])
    c.common(11)
    c.leg('outbound', 0, 0, 3, 0, ['BUD 0km', 'BTS 173km', 'DEB 184km'], ['LGW 41km', 'LTN 44km'], 'AllFares',
          '(none)', cancelled=True)
    c.equal('outbound.warnings', [])
    c.equal('finalStatus', 'Outbound Cancelled: 0 fares from 0 of 3 requests in 1 s.')
    c.equal('requestsToWizz', 3)  # homepage, map, the 503; the retry never went out
    c.log_has('  Outbound: Wizz Air is busy (HTTP 503) — retry 1 of 3')
    # The progress shot at 2.2 s shows the back-off with its countdown (fixed clock: 5.0 s).
    c.progress(1, 1)
    c.equal('progressScreenshots.0', 'Fetching fares:  Wizz Air is busy (HTTP 503) — retry 1 of 3    (1 of 3)')
    c.screenshots(['1-outbound.png', '4-log.png'])
    c.single_cancel_transition(2500)
    c.diag_equal('cancel.busyAtPress', True)
    c.diag_equal('cancel.statusBefore', 'Fetching fares:  Wizz Air is busy (HTTP 503) — retry 1 of 3    (1 of 3)')
    c.search(0, 'outbound', 1)
    c.expect((c.diag('searches.0.durationMs') or 1e9) < 4800,
             'the back-off was not interrupted: %r ms' % c.diag('searches.0.durationMs'))


def notes_suffix(c, path):
    warnings = c.get(path + '.warnings') or []
    return ' %d note(s) in the Log tab.' % len(warnings) if warnings else ''


def check_heavy_common(c):
    c.keys(['startedAt', 'apiUrl', 'airports', 'outbound', 'progressScreenshots', 'finalStatus', 'requestsToWizz',
            'log'])
    c.common(189)
    # The warm-up made every planned request once; the replay from the client's cache made none.
    planned = c.diag('searches.0.wizzRequests') or 0
    c.expect(100 <= planned <= 500, 'warm-up requests: %r' % planned)
    c.search(0, 'warm-up outbound')
    c.search(1, 'outbound', 0)
    c.equal('requestsToWizz', 2 + planned)
    if c.get('outbound') != {'error': 'no result'}:
        c.equal('outbound.requestsPlanned', planned)
        c.equal('outbound.view', 'CheapestPerDestination')
        c.equal('outbound.originAirports', ['BUD 0km'])
    c.screenshots(['1-outbound.png', '4-log.png'])
    return planned


def check_heavy_cached(c):
    """A fully cached replay of about 6000 fares (05 §5): the 40 ms timer keeps ticking."""
    planned = check_heavy_common(c)
    flights = c.get('outbound.flights') or 0
    c.expect(flights >= 4000, 'outbound.flights: only %r fares' % flights)
    c.equal('outbound.requestsCompleted', planned)
    c.equal('outbound.cancelled', False)
    c.equal('finalStatus', 'Outbound Done: %d fares from %d of %d requests in 1 s.%s' %
            (flights, planned, planned, notes_suffix(c, 'outbound')))
    c.equal('outbound.warnings.0', '3 route(s) skipped: Wizz Air shows no upcoming flights on them.')
    c.diag_equal('cancel', None)
    c.diag_equal('grids.outbound.rows', flights)
    c.diag_equal('grids.outbound.resultFlights', flights)


def check_cancel_cached(c):
    """Esc / Cancel during a fully cached replay: one transition and a consistent final table,
    whichever of the cancel and the worker's end comes first."""
    planned = check_heavy_common(c)
    c.single_cancel_transition(0)
    flights = c.get('outbound.flights')
    status = c.get('finalStatus') or ''
    if c.get('outbound') == {'error': 'no result'}:
        # Cancelled before the fetch loop (route map / exchange-rate stage).
        c.equal('finalStatus', 'Cancelled.')
        c.log_has('Outbound: cancelled.')
        c.diag_equal('grids.outbound.rows', 0)
        c.diag_equal('grids.outbound.resultFlights', None)
    elif c.get('outbound.cancelled'):
        c.expect((c.get('outbound.requestsCompleted') or 0) < planned, 'cancelled, yet every request completed')
        c.equal('finalStatus', 'Outbound Cancelled: %d fares from %d of %d requests in 1 s.%s' %
                (flights, c.get('outbound.requestsCompleted'), planned, notes_suffix(c, 'outbound')))
        c.diag_equal('grids.outbound.rows', flights)
    else:
        # The worker had finished before the cancel was handled: the full result stands.
        c.equal('finalStatus', 'Outbound Done: %d fares from %d of %d requests in 1 s.%s' %
                (flights, planned, planned, notes_suffix(c, 'outbound')))
        c.diag_equal('grids.outbound.rows', flights)
    print('cancel during the cached replay ended with: %s' % status)


# --- Generated heavy replay ------------------------------------------------------------------

HEAVY_ORIGIN = 'BUD'
HEAVY_START = datetime.date(2026, 11, 3)
HEAVY_END = datetime.date(2027, 2, 15)  # 105 days: 42 + 42 + 21-day chunks per route
CHUNK_DAYS = 42


def chunks(start, end):
    while start <= end:
        last = min(end, start + datetime.timedelta(days=CHUNK_DAYS - 1))
        yield start, last
        start = last + datetime.timedelta(days=1)


def heavy_timetable(origin, destination, start, end, seed):
    rows = []
    day = start
    index = 0
    while day <= end:
        minutes = 6 * 60 + ((seed * 37 + index * 53) % (16 * 12)) * 5
        when = '%sT%02d:%02d:00' % (day.isoformat(), minutes // 60, minutes % 60)
        amount = 9.99 + ((seed * 7919 + index * 104729) % 23000) / 100.0
        rows.append({
            'departureStation': origin,
            'arrivalStation': destination,
            'departureDate': when,
            'price': {'amount': round(amount, 2), 'currencyCode': 'EUR'},
            'priceType': 'price',
            'departureDates': [{'date': when, 'isCheapestOfTheDay': True}],
            'hasMacFlight': False,
        })
        day += datetime.timedelta(days=1)
        index += 1
    return {'outboundFlights': rows, 'returnFlights': None}


def prepare_heavy(replay_root, work_dir):
    """Writes <work>/replay: every route from Budapest with one fare per day, per chunk."""
    live_map = os.path.normpath(os.path.join(replay_root, '..', '..', 'history', 'live-cache-2026-11-02', 'map.json'))
    shared = os.path.join(replay_root, 'named_destination')
    target = os.path.join(work_dir, 'replay')
    os.makedirs(target)
    with open(live_map, encoding='utf-8') as f:
        cities = json.load(f)['cities']
    origin = next(city for city in cities if city['iata'] == HEAVY_ORIGIN)
    rules = [
        {'comment': 'homepage advertising API 29.23.1', 'urlPrefix': 'https://www.wizzair.com',
         'file': os.path.join(shared, 'home.html')},
        {'comment': 'the frozen live route map (189 airports)', 'urlContains': '/asset/map', 'file': live_map},
        {'comment': 'exchange rates', 'urlContains': 'frankfurter',
         'file': os.path.join(shared, 'fx_frankfurter_v2.json')},
    ]
    for seed, connection in enumerate(sorted(origin['connections'], key=lambda c: c['iata'])):
        destination = connection['iata']
        for start, end in chunks(HEAVY_START, HEAVY_END):
            name = 'tt_%s_%s_%s.json' % (HEAVY_ORIGIN, destination, start.isoformat())
            with open(os.path.join(target, name), 'w', encoding='utf-8', newline='\n') as f:
                json.dump(heavy_timetable(HEAVY_ORIGIN, destination, start, end, seed), f, separators=(',', ':'))
            rules.append({
                'method': 'POST', 'urlContains': '/search/timetableV2',
                'bodyContains': '"departureStation":"%s","arrivalStation":"%s","from":"%s"' %
                                (HEAVY_ORIGIN, destination, start.isoformat()),
                'file': name,
            })
    rules.append({'comment': 'anything else: not served', 'method': 'POST', 'urlContains': '/search/timetableV2',
                  'status': 400, 'body': '{"validationCodes":["InvalidMarket"]}'})
    with open(os.path.join(target, 'replay.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'rules': rules}, f, indent=1)
    return target


HEAVY_FLAGS = ['--from', 'Budapest', '--from-radius', '0', '--to', '', '--out', '2026-11-03:2027-02-15',
               '--warm-cache', '--no-pacing']

SCENARIOS = {
    'named_destination': (['--from', 'Budapest', '--from-radius', '200', '--to', 'London', '--to-radius', '30',
                           '--out', '2026-11-10:2026-11-20', '--ret', '2026-11-12:2026-11-20',
                           '--then-currency', 'HUF'], check_named_destination, None),
    'anywhere_return_from_top': (['--from', 'Budapest', '--from-radius', '0', '--to', '',
                                  '--out', '2026-11-10:2026-12-10', '--return-from-top'],
                                 check_anywhere_return_from_top, None),
    'not_served': (['--from', 'Debrecen', '--from-radius', '0', '--to', 'London', '--to-radius', '30',
                    '--out', '2026-11-10:2026-11-20'], check_not_served, None),
    'same_area': (['--from', 'Budapest', '--to', 'BUD', '--out', '2026-11-10:2026-11-20'], check_same_area, None),
    'over_500': (['--from', 'Budapest', '--from-radius', '1500', '--to', '', '--out', '2026-11-03:2028-11-01'],
                 check_over_500, None),
    'cancel_waiting': (['--from', 'Budapest', '--from-radius', '200', '--to', 'London', '--to-radius', '30',
                        '--out', '2026-11-10:2026-11-20', '--cancel-after-ms', '1500'], check_cancel_waiting, None),
    'cancel_retrying': (['--from', 'Budapest', '--from-radius', '200', '--to', 'London', '--to-radius', '30',
                         '--out', '2026-11-10:2026-11-20', '--cancel-after-ms', '2500'], check_cancel_retrying, None),
    'heavy_cached': (HEAVY_FLAGS, check_heavy_cached, prepare_heavy),
    'cancel_cached': (HEAVY_FLAGS + ['--cancel-after-ms', '0'], check_cancel_cached, prepare_heavy),
}


# --- Start-up crashes (01 §3.2, D-30) ----------------------------------------------------------

CRASH_BOX_END = '\n\nDetails were written to %LOCALAPPDATA%\\WizzairHelper\\errors.log.'
REAL_NOW = r'\[\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\] '
DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
DPI_AWARENESS_UNAWARE = 0
WM_CLOSE = 0x0010  # an MB_OK box ends with IDOK (its button's control ID is IDCANCEL)
MESSAGE_BOX_TEXT_ID = 0xFFFF


def prepare_bad_replay(replay_root, work_dir):
    """A replay.json rule with a field the format does not know: rejected at start-up (D-30)."""
    target = os.path.join(work_dir, 'replay')
    os.makedirs(target)
    with open(os.path.join(target, 'replay.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'rules': [{'urlPrefix': 'https://www.wizzair.com', 'body': '', 'bogus': 1}]}, f)
    return target


# name: (extra flags for the replay dir, prepare, errors.log record pattern, text in the box)
CRASH_SCENARIOS = {
    # Parsing fails before QApplication exists (and before any fixed clock): real local time.
    'crash_bad_fixed_now': (lambda replay: ['--fixed-now', 'not-a-time'], None,
                            REAL_NOW + re.escape("wizz::FormatError: String 'not-a-time' was not recognized as a "
                                                 "valid UTC date and time.\r\n   at "),
                            "String 'not-a-time' was not recognized as a valid UTC date and time."),
    # The replay fails after parsing: the record carries the --fixed-now instant, local zone UTC.
    'crash_bad_replay': (lambda replay: ['--replay', replay, '--fixed-now', FIXED_NOW], prepare_bad_replay,
                         re.escape('[2026-11-02T10:00:00] wizz::ui::ReplayConfigError: ') + r'[^\r\n]*"bogus"',
                         '"bogus"'),
}


ENUM_WINDOWS_PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def user32_functions():
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    user32.EnumWindows.argtypes = [ENUM_WINDOWS_PROC, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindow.argtypes = [wintypes.HWND]
    user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetDlgItemTextW.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.LPWSTR, ctypes.c_int]
    user32.GetDlgItemTextW.restype = wintypes.UINT
    user32.GetWindowDpiAwarenessContext.argtypes = [wintypes.HWND]
    user32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
    user32.AreDpiAwarenessContextsEqual.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.AreDpiAwarenessContextsEqual.restype = wintypes.BOOL
    user32.GetAwarenessFromDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user32.GetAwarenessFromDpiAwarenessContext.restype = ctypes.c_int
    user32.GetDpiAwarenessContextForProcess.argtypes = [wintypes.HANDLE]
    user32.GetDpiAwarenessContextForProcess.restype = ctypes.c_void_p
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    return user32


def find_crash_box(user32, pid):
    """The visible 'WizzairHelper' dialog (#32770) of process `pid`, or None."""
    found = []

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        name = ctypes.create_unicode_buffer(64)
        title = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        user32.GetWindowTextW(hwnd, title, 64)
        if name.value == '#32770' and title.value == 'WizzairHelper':
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(ENUM_WINDOWS_PROC(visit), 0)
    return found[0] if found else None


def process_dpi_awareness_context(user32, pid):
    """The DPI_AWARENESS_CONTEXT of process `pid` (None when it cannot be opened)."""
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    process_query_limited_information = 0x1000
    handle = kernel32.OpenProcess(process_query_limited_information, False, pid)
    if not handle:
        return None
    try:
        return user32.GetDpiAwarenessContextForProcess(handle)
    finally:
        kernel32.CloseHandle(handle)


def real_errors_log():
    """%LOCALAPPDATA%\\WizzairHelper\\errors.log of the user running the tests."""
    buffer = ctypes.create_unicode_buffer(wintypes.MAX_PATH)
    csidl_local_appdata = 0x1C
    ctypes.windll.shell32.SHGetFolderPathW(None, csidl_local_appdata, None, 0, buffer)
    return os.path.join(buffer.value, 'WizzairHelper', 'errors.log')


def undo_real_log_growth(path, size_before, folder_existed, marker):
    """True when the real errors.log grew; a record of this run is removed again (with the
    file and the folder when this run created them)."""
    if not os.path.isfile(path) or os.path.getsize(path) <= (size_before or 0):
        return False
    with open(path, 'rb') as f:
        f.seek(size_before or 0)
        added = f.read()
    if marker.encode('utf-8') in added:
        if size_before is None:
            os.remove(path)  # this run created it
            if not folder_existed and not os.listdir(os.path.dirname(path)):
                os.rmdir(os.path.dirname(path))
        else:
            with open(path, 'r+b') as f:
                f.truncate(size_before)
    return True


def run_crash(args, env, work_dir, replay_root):
    flags, prepare, record_pattern, box_part = CRASH_SCENARIOS[args.scenario]
    out_dir = os.path.join(work_dir, 'out')
    data_root = os.path.join(work_dir, 'data')
    replay = prepare(replay_root, work_dir) if prepare else None
    command = [os.path.abspath(args.exe), '--autotest', out_dir, '--data-root', data_root] + flags(replay)
    real_log = real_errors_log()
    real_size = os.path.getsize(real_log) if os.path.isfile(real_log) else None
    real_folder_existed = os.path.isdir(os.path.dirname(real_log))
    user32 = user32_functions()
    failures = []
    print('running:', subprocess.list2cmdline(command))
    process = subprocess.Popen(command, env=env)
    try:
        box = None
        deadline = time.monotonic() + args.timeout
        while box is None and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
            box = find_crash_box(user32, process.pid)
        if box is None:
            failures.append('no crash box appeared (exit code %r)' % process.poll())
        else:
            # 01 §3 / 06 §4: per-monitor DPI handling before anything is shown. In a per-monitor-v2
            # process Windows creates a MessageBox system-DPI aware (as for every later crash box);
            # in a DPI-unaware one it is bitmap-stretched above 100 %.
            process_context = process_dpi_awareness_context(user32, process.pid)
            if not user32.AreDpiAwarenessContextsEqual(process_context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2):
                failures.append('the process is not per-monitor-v2 DPI aware when the crash box shows '
                                '(DPI_AWARENESS %d)' % user32.GetAwarenessFromDpiAwarenessContext(process_context))
            box_awareness = user32.GetAwarenessFromDpiAwarenessContext(user32.GetWindowDpiAwarenessContext(box))
            if box_awareness == DPI_AWARENESS_UNAWARE:
                failures.append('the crash box is DPI-unaware (bitmap-stretched)')
            text = ctypes.create_unicode_buffer(4096)
            user32.GetDlgItemTextW(box, MESSAGE_BOX_TEXT_ID, text, 4096)
            shown = text.value.replace('\r\n', '\n')
            if not (shown.startswith('Something went wrong:\n\n') and box_part in shown and
                    shown.endswith(CRASH_BOX_END)):
                failures.append('crash box text: %r' % shown)
            user32.PostMessageW(box, WM_CLOSE, 0, 0)
        exit_code = process.wait(timeout=30)
        if box is not None and exit_code != 1:
            failures.append('exit code %d after the crash box, expected 1' % exit_code)
    except subprocess.TimeoutExpired:
        failures.append('WizzairHelper.exe did not end after the crash box was closed')
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if undo_real_log_growth(real_log, real_size, real_folder_existed, box_part):
            failures.append('the record went to the real %s (removed again)' % real_log)

    data_log = os.path.join(data_root, 'errors.log')
    if not os.path.isfile(data_log):
        failures.append('no errors.log in the --data-root profile %s' % data_root)
    else:
        with open(data_log, encoding='utf-8', newline='') as f:
            log = f.read()
        if not re.match(record_pattern, log) or not log.endswith('\r\n\r\n') or log.count('\r\n\r\n') != 1:
            failures.append('errors.log: expected one record matching %r, got %r' % (record_pattern, log[:400]))
        else:
            print('errors.log: %s' % log.splitlines()[0])
    if os.path.exists(os.path.join(out_dir, 'summary.json')):
        failures.append('summary.json written although the start-up failed')
    if failures:
        for failure in failures:
            print('FAILED:', failure)
        return 1
    print('OK: %s' % args.scenario)
    return 0


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--exe', required=True)
    parser.add_argument('--replay-root', required=True)
    parser.add_argument('--scenario', required=True, choices=sorted(list(SCENARIOS) + list(CRASH_SCENARIOS)))
    parser.add_argument('--work-dir', required=True)
    parser.add_argument('--timeout', type=float, default=150)
    parser.add_argument('--scale', help='QT_SCALE_FACTOR for the run, e.g. 1.5 or 2')
    parser.add_argument('--max-gap-ms', type=float, default=100.0,
                        help='limit for the longest gap of the 40 ms progress timer (05 §5: 100 ms)')
    args = parser.parse_args()

    work_dir = os.path.abspath(args.work_dir)
    replay_root = os.path.abspath(args.replay_root)
    shutil.rmtree(work_dir, ignore_errors=True)
    os.makedirs(work_dir)
    env = dict(os.environ)
    env.pop('QT_SCREEN_SCALE_FACTORS', None)
    env.pop('QT_QPA_PLATFORM', None)  # PrintWindow needs the real "windows" platform
    if args.scale:
        env['QT_SCALE_FACTOR'] = args.scale
    else:
        env.pop('QT_SCALE_FACTOR', None)
    if args.scenario in CRASH_SCENARIOS:
        return run_crash(args, env, work_dir, replay_root)

    flags, check, prepare = SCENARIOS[args.scenario]
    out_dir = os.path.join(work_dir, 'out')
    data_root = os.path.join(work_dir, 'data')
    replay = prepare(replay_root, work_dir) if prepare else os.path.join(replay_root, args.scenario)
    command = [os.path.abspath(args.exe), '--autotest', out_dir, '--data-root', data_root, '--replay', replay,
               '--fixed-now', FIXED_NOW, '--diagnostics'] + flags
    print('running:', subprocess.list2cmdline(command), '(QT_SCALE_FACTOR=%s)' % (args.scale or 'unset'))
    try:
        completed = subprocess.run(command, timeout=args.timeout, env=env)
    except subprocess.TimeoutExpired:
        print('FAILED: WizzairHelper.exe did not finish within %.0f s' % args.timeout)
        return 1
    if completed.returncode != 0:
        print('FAILED: exit code %d' % completed.returncode)
        return 1

    summary_path = os.path.join(out_dir, 'summary.json')
    diagnostics_path = os.path.join(out_dir, 'diagnostics.json')
    for path in (summary_path, diagnostics_path):
        if not os.path.isfile(path):
            print('FAILED: no %s in %s' % (os.path.basename(path), out_dir))
            return 1
    with open(summary_path, encoding='utf-8') as f:
        summary = json.load(f)
    with open(diagnostics_path, encoding='utf-8') as f:
        diagnostics = json.load(f)
    checker = Checker(summary, diagnostics, out_dir)
    check(checker)
    checker.diagnostics_common(args.max_gap_ms, args.scale)
    checker.expect(not os.path.exists(os.path.join(data_root, 'settings.json')), 'settings.json written in autotest')
    checker.expect(not os.path.exists(os.path.join(data_root, 'errors.log')), 'errors.log written: the app crashed')
    timer = diagnostics.get('timer', {})
    events = diagnostics.get('uiEvents') or {}
    print('timer: %s runs, %s ticks, longest gap %s ms (UI-thread CPU at most %s ms in one gap; longest gap not held '
          'by other processes %s ms; %s gaps of 100 ms or more held by them), mean %s ms, %s gaps over 50 ms; longest '
          'UI dispatch %s ms (while busy %s ms, CPU %s ms, not held by other processes %s ms); DPR %s; %s' %
          (timer.get('runs'), timer.get('ticks'), timer.get('longestGapMs'), timer.get('longestGapCpuMs'),
           timer.get('longestOwnGapMs'), timer.get('heldGapsOver100Ms'), timer.get('meanGapMs'),
           timer.get('gapsOver50Ms'), events.get('longestMs'), events.get('longestBusyMs'),
           events.get('longestBusyCpuMs'), events.get('longestBusyOwnMs'),
           diagnostics.get('display', {}).get('devicePixelRatio'), diagnostics.get('environment', {}).get('os')))
    if checker.failures:
        for failure in checker.failures:
            print('FAILED:', failure)
        return 1
    print('OK: %s (%d log lines, %d progress screenshots)' %
          (args.scenario, len(summary.get('log', [])), len(summary.get('progressScreenshots', []))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
