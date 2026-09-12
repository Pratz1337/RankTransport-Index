# Third-party code and data

The root MIT license applies to the authors' original HRT-LI code and original
changes. It does not relicense upstream code, patch context, datasets or other
third-party material.

The baseline source trees are not vendored. Obtain the revisions documented in
[the reproduction guide](docs/CURRENT_COMPARISON_REPRODUCTION.md) from upstream.

| Dependency | Measured revision | License information |
|---|---|---|
| [libart](https://github.com/armon/libart) | `301046804af165269e37da6725f5a4aec9ecc881` | BSD 3-Clause; retain upstream notices when obtaining or distributing it. |
| [HOT](https://github.com/speedskater/hot) | `96bf6fb7103b27e50e16a6026db8974c090ee84a` | ISC; the upstream notice is retained in `licenses/HOT-ISC.txt` for the patch excerpts. |
| [LITS](https://github.com/schencoding/lits) | `6f4793dff0dcf66daada49e2f4ae1b1838bfa9cc` | No repository license file was found at this revision. This release does not grant permission to redistribute or relicense LITS. Check upstream terms or obtain permission for your intended reuse. |

The small patches in `scripts/patches/` document the changes made for the
recorded comparisons, rather than distributing complete baseline implementations.
LITS embeds a HOT fork; upstream HOT context in those patches retains its ISC
notice. Other upstream context remains the property of its respective authors.
The root MIT license is not a substitute for those upstream rights.

Common Crawl host keys are not redistributed. Their public source URLs and
checksums are listed in the corpus metadata. Consult
[Common Crawl's terms](https://commoncrawl.org/terms-of-use) when obtaining or
reusing the data. Code licensing does not change data rights.
