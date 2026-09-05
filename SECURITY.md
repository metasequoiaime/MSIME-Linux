# Security Policy

## Supported versions

Security fixes are provided for the latest revision of the `main` branch. Before reporting a problem, confirm that it still occurs with a current checkout built against the pinned `vendor/MetasequoiaImeEngine` submodule.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability or include sensitive proof-of-concept data in public discussions.

Email `suzukaze.haduki@gmail.com` with the subject `MetasequoiaImeLinux security report`. Include the affected commit, the distribution and version, the desktop environment and IBus version, expected and observed behavior, reproduction steps, and the security impact. Attach only the minimum data needed to reproduce the issue; never include real text typed with the input method, Secret Service credentials, API tokens, or other personal data.

Issues in the shared input engine belong to [`MSIME-Engine`](https://github.com/metasequoiaime/MSIME-Engine); report them through the same private channel and say which repository is affected.

The maintainer will coordinate disclosure and remediation with the reporter. Please allow time for a fix to land before publishing technical details.

For ordinary defects and feature requests that have no security or privacy impact, use the repository's public issue tracker.
