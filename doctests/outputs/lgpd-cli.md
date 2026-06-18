# Browsing the Logos Package Catalog with lgpd

[`lgpd`](https://github.com/logos-co/logos-package-downloader) is the Logos **package
downloader** — a small C++/libcurl CLI (no Qt) that browses the online package catalog
and fetches `.lgx` packages. It is the network-facing complement to
[`lgpm`](https://github.com/logos-co/logos-package-manager), the offline local package
manager: `lgpd` finds and downloads packages; `lgpm` installs and inspects them.

This doc-test builds **this** `lgpd` commit and tours the parts of its surface that are
deterministic without depending on the live catalog being up:

1. Build the CLI and confirm `--version` / `--help`.
2. Scaffold a repository config with `config init`, then read it back with
   `config show` / `config path`.
3. List the configured repositories — the built-in default repository is always present
   (its URL is compiled into `lgpd`), asserted via the structural `[default]` marker.
4. Search the catalog for a term with no matches (a graceful empty result), and show —
   without asserting on — a live `list`.

The repository-mutation and live-catalog commands hit the network, so this doc-test
shows them but only asserts on output that is stable offline. Every command is the real
binary built from the commit under test, so a green run is evidence the downloader's CLI
still works.

**What you'll build:** This commit of the `lgpd` downloader, a scaffolded `repositories.json`, and a tour of its catalog and repository-management commands.

**What you'll learn:**

- How to build `lgpd` from its flake and confirm the version
- How to scaffold and inspect a repository config (`config init` / `show` / `path`)
- How the built-in default repository always appears in `repo list` (its URL is compiled in)
- How `search` reports an empty result, and why network-dependent output isn't asserted on

## Prerequisites

- **Nix** with flakes enabled (used to build the `lgpd` CLI).
- A Linux or macOS machine.

---

## Step 1: Build the lgpd CLI

`lgpd` ships as a C++ CLI. Build it straight from the flake's `#cli` output and link
the result as `./lgpd`, so the binary lands at `./lgpd/bin/lgpd`.

> The `` in the URL pins the build to the commit under test: the doc-test
> runner expands it to a concrete ref (locally this checkout's `HEAD` — see
> `run.sh`; in CI the commit being tested). With no pin it falls back to the latest
> `master`. Developing against a local checkout? Replace the GitHub reference with
> `.`, e.g. `nix build '.#cli' -o lgpd`.

### 1.1 Build lgpd

```bash
nix build 'github:logos-co/logos-package-downloader#cli' -o lgpd
```

### 1.2 Confirm the version

```bash
lgpd --version
```

### 1.3 Show the command surface

```bash
lgpd --help
```

---

## Step 2: Scaffold a repository config

Repository-management commands operate on a config file passed with `--config`.
`config init` writes an empty one you can then inspect.

### 2.1 Create an empty config

```bash
lgpd config init ./repositories.json
```

### 2.2 Print the active config path

```bash
lgpd --config ./repositories.json config path
```

### 2.3 Show the resolved config

```bash
lgpd --config ./repositories.json config show
```

A freshly-initialised config carries an empty user `repositories` array — the
built-in default repository (below) is compiled in, not stored here.

---

## Step 3: List the configured repositories

`repo list` shows every repository the catalog is assembled from. Even with an empty
config, the built-in default repository is always present — its URL is compiled into
`lgpd`, so the entry itself appears without any config. (Its human-readable *name* is
resolved from the repository's `logos-repo.json` over the network, so we assert on
the always-present structural markers — the `Configured repositories:` header and the
`[default]` tag — rather than the fetched name.)

### 3.1 repo list

```bash
lgpd --config ./repositories.json repo list
```

### 3.2 Repo mutations require --config (error path)

Running a `repo` mutation without `--config` is a usage error — `lgpd` says so
and points you at `config init`. We add `|| true` so the doc-test can assert on
the message of this deliberately failing command.

```bash
lgpd repo add https://example.com/my/logos-repo.json
```

---

## Step 4: Search the catalog

`search` filters the merged catalog by name/description. Searching for a term that
matches nothing returns a clean, offline-safe empty result — the assertion below
doesn't depend on what packages the live catalog currently holds.

### 4.1 Search for a non-existent package

```bash
lgpd search zzz-no-such-package-xyz
```

### 4.2 Browse the live catalog (shown, not asserted)

`lgpd list` prints whatever the live catalog currently advertises — useful
interactively, but its content changes over time, so this doc-test runs it for
illustration and does not assert on the output. `|| true` keeps the step green
regardless of network availability.

```bash
lgpd list
```

---

## Recap

You built this commit of `lgpd` and toured its offline-deterministic surface:

| Command | What it does |
|---|---|
| `config init <path>` | scaffold an empty repository config |
| `config show` / `config path` | inspect the resolved config |
| `repo list` | list repositories (built-in default always present) |
| `search <query>` | filter the catalog (empty result shown here) |
| `list` / `download` / `info` | live-catalog commands (network-dependent) |

Because `lgpd` is built from the commit under test, a green run proves the
downloader's CLI still builds and behaves.
