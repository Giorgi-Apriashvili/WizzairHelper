WizzairHelper
=============

Finds the cheapest Wizz Air flights from airports near a place you choose,
and combines outbound and return fares into round trips.

Running
-------
Start WizzairHelper.exe. Nothing else needs to be installed: the Qt libraries,
libcurl and the Microsoft Visual C++ runtime are in this folder. Windows 10
(version 1903 or later) or Windows 11, x64.

The portable ZIP can be unpacked anywhere. The installer puts the same files
in %LOCALAPPDATA%\Programs\WizzairHelper for the current user, with a
Start-menu entry; remove it in Settings > Apps (or unins000.exe in that
folder). WizzairHelper.exe --version prints the version.

Your data
---------
Settings, the error log and the caches (route map, exchange rates, place
lookups) are stored in %LOCALAPPDATA%\WizzairHelper. Delete that folder to
start afresh. The program writes nowhere else.

Network
-------
WizzairHelper talks to wizzair.com (fares), photon.komoot.io and
nominatim.openstreetmap.org (places) and the exchange-rate services listed in
THIRD-PARTY-NOTICES.txt, through the Windows proxy settings. Requests to
Wizz Air are spaced about one second apart and a search is limited to 500
requests.

Licenses
--------
WizzairHelper is published under the MIT license (LICENSE.txt). The libraries
it ships with are listed in THIRD-PARTY-NOTICES.txt; their licenses are in the
licenses folder.
