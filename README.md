# znc-identserver
## This module is based on the great work done by [KiNgMaR](https://github.com/KiNgMaR) on his [IdentServer module](https://github.com/KiNgMaR/znc/blob/msvc/win32/extra_modules/identserver.cpp)

ZNC module based on the KingMar's identserver module; it has been extended, refactored and fixed. 

## Differences from the original
* Public listening port defaults to 9113 because znc should never run as root and so binding directly to port 113 couldn't be possible because it's in the privileged port range. If you run znc as root and want to listen on port 113 specify it as the module's load argument.
* Listening port can be changed by using the desired port number as an argument in webadmin or on `/msg *status loadmod indentserver [port]`.
* Fixed a bug when you configured a specific network ident and the module would still reply with the user's global ident setting.
* When connecting to a server prints the ident request and reply of that specific server
* Code refactored for maintainability and better support of recent znc APIs.

## Installation
Build the module with `znc-buildmod identserver.cpp` or if using the official docker image put the identserver.cpp file in your /znc-data/modules directory and restart the container.

## Supported commands
Sending a privmsg to `*identserver` with `Status` in it will show the actual state of the IdentD server and informations on the last received query and the reply sent.

