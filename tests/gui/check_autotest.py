"""End-to-end check of the hidden self-test mode (01 §17, 05 §5, DECISIONS D-30).

Runs the real WizzairHelper.exe as

    WizzairHelper.exe --autotest <work>/out --data-root <work>/data
                      --replay <replay-root>/<scenario> --fixed-now 2026-11-02T10:00:00Z <scenario flags>

on an isolated profile and a replayed network (tests/replay/<scenario>), then checks
summary.json and the screenshots. Run with `python -I` (no site or user paths).

summary.json carries no booking URLs (01 §17.3, 01-digest C-07); the links the grids
open are asserted by the in-process GUI tests (ControllerFlows.*).
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

FIXED_NOW = '2026-11-02T10:00:00Z'
STARTED = ('[10:00:00] WizzairHelper started. Fares: wizzair.com (unofficial website API). Geocoding: Photon / '
           'Nominatim, data © OpenStreetMap contributors. Exchange rates: Frankfurter / currency-api.')
ROUTE_MAP_LOG = '[10:00:00] Route map: {} airports, API https://be.wizzair.com/29.23.1/Api.'
KEY_ORDER = ['startedAt', 'apiUrl', 'airports', 'outbound', 'return', 'roundTrips', 'afterCurrencySwitch',
             'exception', 'progressScreenshots', 'finalStatus', 'requestsToWizz', 'log']


class Checker:
    def __init__(self, summary, out_dir):
        self.summary = summary
        self.out_dir = out_dir
        self.failures = []

    def expect(self, condition, message):
        if not condition:
            self.failures.append(message)

    def equal(self, path, expected):
        actual = self.get(path)
        self.expect(actual == expected, '%s: expected %r, got %r' % (path, expected, actual))
        return actual

    def get(self, path):
        value = self.summary
        for part in path.split('.'):
            if isinstance(value, dict):
                value = value.get(part)
            elif isinstance(value, list) and part.isdigit() and int(part) < len(value):
                value = value[int(part)]
            else:
                return None
        return value

    def keys(self, expected):
        self.equal_list('top-level keys', list(self.summary.keys()), expected)
        self.expect([k for k in self.summary if k in KEY_ORDER] == [k for k in KEY_ORDER if k in self.summary],
                    'top-level key order differs from 01 §17.2')

    def equal_list(self, what, actual, expected):
        self.expect(actual == expected, '%s: expected %r, got %r' % (what, expected, actual))

    def log_has(self, line):
        lines = self.summary.get('log', [])
        self.expect(any(entry == '[10:00:00] ' + line for entry in lines), 'log line missing: %r' % line)

    def leg(self, path, flights, rows, planned, completed, origins, destinations, view, current, fx=True):
        self.equal(path + '.flights', flights)
        self.equal(path + '.gridRows', rows)
        self.equal(path + '.requestsPlanned', planned)
        self.equal(path + '.requestsCompleted', completed)
        self.equal(path + '.cancelled', False)
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

    def progress(self, at_least):
        shots = self.summary.get('progressScreenshots', [])
        self.expect(at_least <= len(shots) <= 4, 'progressScreenshots: expected %d..4 entries, got %d' % (at_least, len(shots)))
        for status in shots:
            self.expect(status.startswith('Fetching fares'), 'progress status: %r' % status)
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


SCENARIOS = {
    'named_destination': (['--from', 'Budapest', '--from-radius', '200', '--to', 'London', '--to-radius', '30',
                           '--out', '2026-11-10:2026-11-20', '--ret', '2026-11-12:2026-11-20',
                           '--then-currency', 'HUF'], check_named_destination),
    'anywhere_return_from_top': (['--from', 'Budapest', '--from-radius', '0', '--to', '',
                                  '--out', '2026-11-10:2026-12-10', '--return-from-top'],
                                 check_anywhere_return_from_top),
    'not_served': (['--from', 'Debrecen', '--from-radius', '0', '--to', 'London', '--to-radius', '30',
                    '--out', '2026-11-10:2026-11-20'], check_not_served),
    'same_area': (['--from', 'Budapest', '--to', 'BUD', '--out', '2026-11-10:2026-11-20'], check_same_area),
    'over_500': (['--from', 'Budapest', '--from-radius', '1500', '--to', '', '--out', '2026-11-03:2028-11-01'],
                 check_over_500),
}


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--exe', required=True)
    parser.add_argument('--replay-root', required=True)
    parser.add_argument('--scenario', required=True, choices=sorted(SCENARIOS))
    parser.add_argument('--work-dir', required=True)
    parser.add_argument('--timeout', type=float, default=150)
    args = parser.parse_args()

    flags, check = SCENARIOS[args.scenario]
    out_dir = os.path.join(args.work_dir, 'out')
    data_root = os.path.join(args.work_dir, 'data')
    shutil.rmtree(args.work_dir, ignore_errors=True)
    os.makedirs(args.work_dir)
    command = [args.exe, '--autotest', out_dir, '--data-root', data_root,
               '--replay', os.path.join(args.replay_root, args.scenario), '--fixed-now', FIXED_NOW] + flags
    print('running:', subprocess.list2cmdline(command))
    try:
        completed = subprocess.run(command, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print('FAILED: WizzairHelper.exe did not finish within %.0f s' % args.timeout)
        return 1
    if completed.returncode != 0:
        print('FAILED: exit code %d' % completed.returncode)
        return 1

    summary_path = os.path.join(out_dir, 'summary.json')
    if not os.path.isfile(summary_path):
        print('FAILED: no summary.json in ' + out_dir)
        return 1
    with open(summary_path, encoding='utf-8') as f:
        summary = json.load(f)
    checker = Checker(summary, out_dir)
    check(checker)
    checker.expect(not os.path.exists(os.path.join(data_root, 'settings.json')), 'settings.json written in autotest')
    checker.expect(not os.path.exists(os.path.join(data_root, 'errors.log')), 'errors.log written: the app crashed')
    if checker.failures:
        for failure in checker.failures:
            print('FAILED:', failure)
        return 1
    print('OK: %s (%d log lines, %d progress screenshots)' %
          (args.scenario, len(summary.get('log', [])), len(summary.get('progressScreenshots', []))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
