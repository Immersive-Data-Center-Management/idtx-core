# Third-party test-only dependencies

## doctest

This directory hosts the [doctest](https://github.com/doctest/doctest)
single-header (`doctest.h`), tag **v2.4.11**.

The header is deliberately not vendored via vcpkg because doctest is
truly header-only and does not need to be built/linked. Grab it from:

```
https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```

and drop the file into this directory (`tests/thirdparty/doctest.h`).
After that, `scons tests=1` will build the test suite.