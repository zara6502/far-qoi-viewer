This project intentionally does not bundle Far Manager's plugin.hpp.

Use the current Far Manager source tree:

https://github.com/FarGroup/FarManager

The build expects:

    <FarManager source>/far/plugin.hpp

This avoids compiling against a stale copy of the Far plugin ABI.
