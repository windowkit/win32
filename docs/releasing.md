# Releasing

Every release after the first is a pull request that gets merged. The first one
is done by hand, because npm's trusted publishing has to be configured on a
package that already exists.

## What ships

A tarball with the JavaScript, the C++ sources, `binding.gyp`, and **two
binaries**:

```
prebuilds/win32-x64/win32.node
prebuilds/win32-arm64/win32.node
```

npm resolves a package per platform, not per architecture, so both ride in the
same tarball and `scripts/install.js` picks the one that matches at install
time. Without a match it falls back to `node-gyp rebuild`, and if that fails it
*warns rather than failing the install* — this package is an optional
dependency of react-x11, and the renderer's answer to a missing backend is to
use another one.

Each binary is built on a runner of its own architecture. Nothing
cross-compiles: MSVC will happily produce an ARM64 binary from an x64 host, but
that binary would never have been run before it was published, and the machine
it fails on is the one that cannot build from source. `scripts/check-arch.js`
reads the PE header and refuses a binary filed under the wrong architecture.

## The first release, by hand

You need an `@windowkit` scope on npm and push access to the repository.

1. **Get both binaries.** Push the branch and let `ci` run, then download the
   `prebuild-win32-x64` and `prebuild-win32-arm64` artifacts and unpack them:

   ```
   prebuilds/win32-x64/win32.node
   prebuilds/win32-arm64/win32.node
   ```

   Building locally works too, but only for the architecture you are on —
   `npm run build:prebuild` files the host's binary and nothing else, so a
   release packed that way is missing an architecture.

   Either way, replace whatever is in `prebuilds/` rather than trusting it. A
   working copy never loads its own prebuild — `index.js` prefers `build/` —
   so one left over from an earlier build passes every local test and is still
   what `npm publish` packs.

2. **Check what the tarball would contain.**

   ```bash
   npm pack --dry-run
   ```

   Both `prebuilds/win32-*/win32.node` entries must be in the list. They are
   git-ignored, which does not exclude them from the tarball — `files` in
   package.json decides that — but it does mean a clean checkout has neither.

3. **Publish.**

   ```bash
   npm publish --access public
   ```

   `--access public` is needed the first time for a scoped package; afterwards
   it is the default for that package. No `--provenance` here: provenance comes
   from a CI run, and this one is from a laptop.

4. **Turn on trusted publishing.** On npmjs.com, under the package's Settings →
   Publishing access, add a trusted publisher:

   - Repository: `windowkit/win32`
   - Workflow: `.github/workflows/release-please.yml`
   - Environment: leave empty

   This is the step that needs the package to exist, and it is why step 3 is
   manual. From here on `npm publish` in CI authenticates with the workflow's
   OIDC token and there is no `NPM_TOKEN` anywhere.

5. **Tell release-please where it is starting from.** `.release-please-manifest.json`
   holds the current version:

   ```json
   { ".": "0.0.1" }
   ```

   Set it to whatever you published, commit, push.

   Then tag the commit that was published. The manifest says *which* version
   is out; only a tag says *where* it is:

   ```bash
   git tag v0.0.1 <sha>
   git push origin v0.0.1
   ```

   Without the tag release-please finds no boundary, and the first changelog it
   writes credits the next release with every commit since the scaffold.

## Every release after that

1. Merge work into `main` with [conventional commits](https://www.conventionalcommits.org):
   `feat:` for a minor bump, `fix:` for a patch, `feat!:` or a `BREAKING CHANGE:`
   footer for a major. While the version is below 1.0.0, `feat:` bumps the patch
   and `feat!:` bumps the minor — `bump-minor-pre-major` in
   `release-please-config.json`.

2. release-please keeps a pull request open titled `chore(main): release <version>`.
   It holds the version bump and the changelog entry. Review it like any other.

3. Merge it. That tags, creates the GitHub release, builds both binaries on
   their own runners, attaches them to the release, and publishes to npm with
   provenance.

Nothing else is a release. A tag pushed by hand does not publish, and neither
does a green `ci` run.

## When a release goes wrong

- **A runner for an architecture is missing.** `windows-11-arm` is newer than
  the x64 runner and not on every plan. In `ci` the ARM64 legs are
  `continue-on-error`, so they report without gating; in the release they are
  not, because a release that quietly dropped an architecture is worse than a
  release that stopped. If you need to ship without it, publish by hand.

- **`npm publish` says the package is not configured for trusted publishing.**
  Step 4 above was skipped, or the workflow path in the npm settings does not
  match this file's path exactly.

- **A consumer reports a build on install.** That is the fallback working:
  their architecture had no prebuild. Check the release's assets.
