# A rev.ng fork for large binaries

This fork of [rev.ng](https://rev.ng) makes its whole pipeline, from lifting to C emission, complete on binaries with over 100,000 functions, and fixes wrong or crashing decompiled output it found on the way. [FORK.md](FORK.md) states its goals and [FORK-CHANGES.md](FORK-CHANGES.md) lists every change, grouped by what it fixes, with links to the commits, their tests and their prospects for upstream. Below the line is upstream's README, whose build instructions apply unchanged.

---

# The rev.ng binary analysis framework and decompiler

Welcome to the [rev.ng](https://rev.ng) project!

rev.ng is an open source binary analysis framework based on LLVM and QEMU.
It features a full-fledged decompiler.
It's developed by [rev.ng Labs](https://rev.ng/about) in the open.

## Getting started

Head to the [online documentation](https://docs.rev.ng) to get started ([source](share/doc/revng/index.md)).

## Getting in touch

You can get in touch with us through the following means:

* [X/Twitter](https://twitter.com/_revng), for the latest news about the project;
* [Discord](https://discord.gg/wEQtgKJxcX), a place for chatting for users and developers;
* [Discourse](https://discuss.rev.ng/), for user discussions;
* [GitHub](https://github.com/revng), for development issues and PRs;
* [E-mail](mailto:info@rev.ng), for anything else;

## License

The rev.ng binary analysis framework and decompiler is Free Software.

rev.ng depends on certain on GPLv2 components (in particular, QEMU), so the project is [GPLv2-licensed as a whole](LICENSE).
However, the individual files are licensed under the [MIT license](LICENSE.mit).
