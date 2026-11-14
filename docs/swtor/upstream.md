# Taking upstream's changes

How `swtor-on-mac` takes `dttdrv/mullion`'s `main`: in batches I choose, each read before it is merged, each merged
with a merge commit. Nothing goes the other way. No agent opens, comments on or offers anything in `dttdrv/mullion`,
`3Shain/dxmt` or athei's repositories, not even a finding; a finding upstream should hear about goes to me, and I
decide ([AGENTS.md](../../AGENTS.md)).

Why batches: upstream's `main` moves fast, its maintainer merges his own work without review, no GPU test runs in its
CI, and a converter change there doesn't always raise `AIRCONV_VERSION`. Tracking `main` would bring every regression
the day it lands. Never updating would lose real fixes.

## 1. Where upstream's commits come from

- The fork's `main` mirrors upstream's `main` and gets nothing else. Only GitHub's fork sync moves it:

  ```zsh
  gh repo sync jdrly/mullion --branch main
  ```

  It copies upstream's commits into the fork and can't push anywhere else.
- No checkout has a remote for upstream. `gh repo clone` adds one named `upstream`: remove it
  (`git remote remove upstream`). `git remote -v` lists only `origin`, `jdrly/mullion`.
- A checkout that needs upstream's objects before the sync fetches them by URL, which makes no remote:

  ```zsh
  git fetch https://github.com/dttdrv/mullion.git main   # lands in FETCH_HEAD
  ```

## 2. Choose the batch

A batch is one commit of `main` I name: an upstream release tag, or a commit that carries a fix I need. Never
"whatever is newest". Below, `$SHA` is that commit and `$BASE` is `origin/swtor-on-mac`;
`git merge-base $SHA $BASE` is the last batch, `$LAST`.

```zsh
git fetch origin
SHA=<commit>; BASE=origin/swtor-on-mac; LAST=$(git merge-base $SHA $BASE)
git log --oneline --no-merges $LAST..$SHA      # the commits
git log --format='%h %p %s' --merges $LAST..$SHA   # upstream's merges
git diff --stat $LAST $SHA                     # the files
```

## 3. Read it before merging

A research lane, not the implementer of the merge, writes `runs/lane-<id>/batch-<short sha>.md` in my private
workspace. It stays there: it reviews someone else's code by name, and the fork is public. It holds:

- every commit of the range, grouped by area;
- the high-path commits read line by line ([AGENTS.md](../../AGENTS.md) § High paths: the bridge, the converter,
  lifetimes, anything that hands the GPU an address or a length, the shader cache);
- for each commit: what it changes, whether the shader code the converter generates changes, whether it touches a
  line the fork changed, and a verdict: **take**, **take with a fork switch** whose default is the old behaviour, or
  **leave out** by reverting it inside the batch;
- upstream's merge commits that carry changes of their own: `git show --remerge-diff $m` prints nothing for a clean
  one;
- the `tests/breaks` patches the batch stops from applying (§ 5);
- what upstream merged without review, which today is all of it.

## 4. Merge

On a branch `chore/sync-upstream-<short sha>` off `swtor-on-mac`:

```zsh
git switch -c chore/sync-upstream-$(git rev-parse --short=7 $SHA) $BASE
git merge --no-ff $SHA
M=$(git rev-parse HEAD)   # once the merge is committed
```

Never rebase or squash a batch. Resolve each conflict keeping both sides' intent, and the fork's switches as they are.
These resolutions are fixed:

- `.github/workflows/ci.yml` and `game-reports.yml` stay deleted ([README.md](README.md) § CI). A change upstream
  made to either stops the merge with a modify/delete conflict: `git rm` the file. The merge adds nothing else to
  `.github/workflows/`; `swtor.yml` is the only file there.
- The fork's templates stay: `.github/PULL_REQUEST_TEMPLATE.md`, `.github/ISSUE_TEMPLATE/config.yml`, `task.yml`
  and `bug_report.yml` as they are on `swtor-on-mac` (`git checkout --ours`), and upstream's numbered issue
  templates stay deleted.
- A list both sides added to, such as the test list of `tests/d3d12/meson.build`, keeps every entry of both.

The merge commit's body lists each conflict and its resolution, and says an AI agent made it, when one did. A change
the fork needs because of the batch (a renamed host test in `swtor.yml`, a fork test that has to follow an upstream
rule) goes in its own commit after the merge, on the same branch.

If a commit is left out, revert it after the merge commit (`git revert -m 1` for an upstream merge commit,
`git revert` for one of its own), so the next batch still merges from upstream's history.

## 5. Check the merge

Each check's output goes into the pull request. `$M` is the merge commit.

**Files only upstream changed equal upstream's.**

```zsh
for f in ${(f)"$(comm -23 <(git diff --name-only $LAST $SHA | sort) <(git diff --name-only $LAST $BASE | sort))"}; do
  git diff --quiet $SHA $M -- $f && echo "= $f" || echo "DIFF $f"
done
```

Every line says `=`, except files deleted on purpose (§ 4).

**Files both sides changed.** For each, upstream's change came in whole and the fork's stayed whole when the two
patch-ids match both ways:

```zsh
pid() { git patch-id --stable | cut -d' ' -f1 }
for f in ${(f)"$(comm -12 <(git diff --name-only $LAST $SHA | sort) <(git diff --name-only $LAST $BASE | sort))"}; do
  up=$([[ $(git diff $BASE $M -- $f | pid) == $(git diff $LAST $SHA -- $f | pid) ]] && echo same || echo differs)
  fork=$([[ $(git diff $SHA $M -- $f | pid) == $(git diff $LAST $BASE -- $f | pid) ]] && echo same || echo differs)
  echo "$f upstream=$up fork=$fork"
done
```

A file with `differs` had a conflict or a change on lines both sides touched: the pull request shows its
resolution.

**The codegen id.** Any change to the converter's inputs gives a new id, and every player converts every shader
again once ([caches.md](caches.md)). Compare the source part of the id before and after, with the versions fixed:

```zsh
S=$(mktemp -d); git archive $BASE | tar -x -C $S
for t in $S .; do python3 -I tools/codegen_id.py --llvm-version 0 --metal-version 0 $t; done
```

Equal ids mean no converter input changed. If they differ, the pull request says "every player converts shaders
again" and the next release's notes say so too. The build's own id is in its `VERSION`.

**`AIRCONV_VERSION`**, before and after, for the record:

```zsh
git grep 'define AIRCONV_VERSION' $BASE $M -- src/airconv/airconv_public.h
```

**The controls.** List the `tests/breaks` patches that applied before the merge and don't after:

```zsh
for p in tests/breaks/*.patch; do
  now=$(git apply --check $p 2>/dev/null && echo ok || echo fails)
  before=$( (cd $S && [[ -f $p ]] && { git apply --check $p 2>/dev/null && echo ok || echo fails } || echo new) )
  [[ $now != $before ]] && echo "$p before=$before now=$now"
done
```

A control that stopped applying can't fail its test at the gate (`tests/faults.py` reports it as "does not apply").
The pull request names each.

**Upstream's workflows stay off.**

```zsh
ls .github/workflows                     # swtor.yml only
gh api repos/jdrly/mullion/actions/workflows --jq '.workflows[] | "\(.name) \(.state)"'
```

**No remote for upstream.** `git remote -v` in every checkout on the machine lists no `dttdrv/mullion`.

**The build.** The `SWTOR` workflow is green on the pull request's head. Its host tests run by name (`'host_*'`): if
the batch renamed or added a host test, `swtor.yml` follows in the commit after the merge. Every
`tests/breaks/host_*.patch` runs there too, upstream's included, so a host break the batch added that its test doesn't
notice, or one the batch made stale, fails the run ([README.md](README.md) § CI).

## 6. The pull request

- Title: `Upstream: Merge Mullion's main at <short sha>`, unless a private ticket gives one. The fork's template,
  draft until done.
- "Needs the Mac run" at the top: a batch brings other people's code on every high path at once, and the runner has
  no Apple GPU.
- The body lists each upstream commit with its one-line verdict, each conflict with its resolution, and the checks'
  output (§ 5). It says whether the codegen id changed.
- I merge it with **Create a merge commit**, never squash or rebase, so upstream's commits stay ancestors and the
  next batch starts from them. Afterwards `git branch -r --contains $SHA` lists `origin/swtor-on-mac`.

## 7. The gate

A batch reaches a build players can get only after the GPU gate (private ticket 15) has passed on the commit that
build is made from ([README.md](README.md) § Releases), with the batch and everything after it on the branch: its
follow-up commits and reverts (§ 4) included. The gate is the suite under Metal's validation on a rented Mac, the
controls failing, and the differences from the fork's list of known failures on that Mac explained line by line. A
new failure blocks the batch, or the commit that causes it is reverted inside the batch (§ 4). A revert or a fix
after a gate run makes another candidate, and the gate runs again on it.

## How the fork keeps batches cheap

The fork's changes live in new files where they can (`docs/swtor/`, `tests/swtor/`, `.github/workflows/swtor.yml`,
`.github/swtor/`, its own headers and tests), touch upstream's lines as narrowly as they can, and never reformat
upstream's code. Upstream's README, CONTRIBUTING and `docs/` stay as upstream has them.
