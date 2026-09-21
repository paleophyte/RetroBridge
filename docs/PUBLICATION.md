# Source publication

The cleaned publication history is the source-release candidate. Do not publish
the original/private development checkout or merge its old history back into
this one: it retains removed credentials and vendor material. Publish Git refs
from the cleaned checkout, or use `git archive` for a source ZIP. Copying the
entire working directory can include ignored SDKs, credentials and build output.

The September 21, 2026 pass covers source and documentation, not approval of
compiled binaries. [BINARY_RELEASE.md](BINARY_RELEASE.md) lists the remaining
runtime/SDK distribution requirements. The [audit](PUBLICATION_AUDIT.md)
retains platform limitations and dated live-test evidence. No new guest
deployment, reboot or invasive live test is part of this publication pass.

## Contents and disclosure

- Original code is MIT; third-party notice files retain their own terms.
- Apple/Novell build inputs, operating-system media, guest DLLs, private
  configuration, and agent binaries are not tracked. Local SDK setup verifies
  supplied inputs; a source checkout cannot build every platform unaided.
- Examples use private-network sample addresses and explicit token placeholders.
  Generate a unique token per guest; never deploy the literal placeholder.
- Non-secret Git authorship, historical lab addresses, machine names and local
  paths are deliberately retained. Current installation examples are generic.
  Historical investigations and diagnostic scripts are records of particular
  lab setups, not portable installation instructions.
- Native compatibility is bounded by tested guests/toolchains. Win7 interactive
  self-update/autostart, DOS TSR work, and the other documented platform limits
  are follow-ups; they are not hidden promises of a source release.

## Repeat the checks for the commit being published

1. Check `git status --short` and `git for-each-ref`. Review every branch/tag
   intended for publication; do not push backup refs or use `git push --mirror`.
2. Run `git fsck --full --no-reflogs` and a full-history scanner, for example
   `gitleaks git --log-opts="--all --full-history" --redact`. Retain its report
   privately. A scanner is not proof that arbitrary secrets never existed.
   The lab review also compared current/retired known credential values against
   every local Git object, including unreachable objects, without publishing
   those values, and checked removed vendor payload fingerprints.
3. Clone through Git's transport with `git clone --no-local --single-branch
   --branch master --no-tags <clean-repository> <new-directory>`. Verify there
   are no object alternates or ignored dependencies copied from the original
   workspace. Check that example configuration loads and missing SDK inputs
   produce the documented setup error.
4. Create a fresh Python 3.12 virtual environment; install
   `mcp-server/requirements.txt` and run `python -m pip check`. The final pass
   resolves MCP 2.2.0 and Pillow 12.3.0. MCP 2.0.0 was also tested earlier;
   requirements constrain the bridge to the MCP 2.x server API. These versions
   are validation evidence, not a frozen distribution of third-party packages.
5. Run the README's host test commands with the required compiler. Exercise a
   fresh stdio MCP session: initialize, enumerate tools, list the example
   inventory without exposing tokens, and check the inventory-only rejection.
   Do not call example network addresses merely to check configuration loading.
6. Verify notice-file hashes against `binary-provenance.json`, relative document
   links, and the archive allowlist. Some recorded binary/toolchain fingerprints
   describe the earlier diagnostic build, not executable assets in this source
   release. The notice files force LF endings so their hashes survive Windows
   checkouts.

The final review's commit ID, scan results and archive checksum are kept outside
the commit being verified. That avoids changing the candidate merely to write
its own hash into its documentation. Re-run these checks if its contents change.

## Publish or export

Create an empty GitHub repository, then from the cleaned checkout configure its
URL as `origin` and push the reviewed `master` branch. Do not reuse a remote
whose branch still contains the removed private history without reviewing how
that remote will be replaced. This preparation does not push or create a release.

For an optional source ZIP, choose an output path outside the checkout:

```text
git archive --format=zip --prefix=RetroBridge/ --output=<source-zip-path> HEAD
```

List the resulting ZIP and check its contents against `git ls-tree -r HEAD`.
Do not add locally built binaries or SDK archives to it. Publish its SHA-256
alongside it if distributing the ZIP, and retain the exact source commit ID.
