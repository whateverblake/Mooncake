# AGENTS.md

## `docs/` Directory Changes

- Before modifying files under `docs/`, read `docs/AGENTS.md`.

## Personal Mooncake Documentation and Blog

- The user's personal website and Mooncake documentation repository is
  https://github.com/whateverblake/whateverblake.github.io.
- The published website is https://whateverblake.github.io/.
- Keep personal Mooncake articles and debugging write-ups in the `mooncake/`
  folder of that website repository, separate from this Mooncake source repository.
- Write articles in Markdown; GitHub Pages publishes the website from `main`.

## Documentation Writing Style

- English is the user's second language. Write documentation and blog articles
  in simple, easy-to-understand English.
- Use short sentences, common words, and clear step-by-step instructions.
- Explain technical terms and abbreviations when first used. Keep commands,
  file paths, and technical details accurate.
- Avoid idioms, unnecessary jargon, and complicated sentence structures.
- Use concrete examples and explain what each command does and where to run it.
- Make instructions self-contained so readers can repeat the process in their
  own environment. Do not rely on existing agent-created helper scripts such
  as `lab.sh` to explain setup, building, or debugging.
- Show the underlying commands and required configuration. Explain how to
  create each required file, what each step does, and which paths or settings
  readers need to change for their own machine.
- If a helper script is mentioned as an optional shortcut, first explain the
  complete process without it. Do not assume readers have the local debug lab
  files or other unpublished code.

## Pull Request Guidelines

- Follow `CONTRIBUTING.md` for PR title prefixes, RFC expectations, and
  contribution workflow.
- Before opening a PR for nontrivial work, check whether an existing issue or
  open PR already covers the same change. If the work overlaps, explain the
  difference instead of duplicating it.
- Do not open low-value busywork PRs for isolated typo, style, or mechanical
  changes unless they are part of a substantive requested change.
- Use `.github/pull_request_template.md` when preparing a PR, and fill in the
  relevant sections for description, module, type of change, testing,
  checklist, and AI assistance disclosure.
- For AI-assisted changes, make sure the human submitter has reviewed every
  changed line and can defend the change end-to-end.
- Before handoff, run pre-commit on the files touched by the change when the
  toolchain is available (see `CONTRIBUTING.md` for the PR-scoped
  `pre-commit run --files ...` command). Do not use
  `pre-commit run --all-files` for routine PRs; if it rewrites unrelated
  files, leave those edits out of the PR.
- Keep PRs lean: review `git diff` before staging, and include only changes
  required for the requested task.

## Local Mooncake Debug Lab

- Use `debug-lab/compose.yaml` for the Docker development environment.
- Container name: `mooncake-debug`.
- SSH mapping: `127.0.0.1:33333:22` (Mac port `33333` to container port `22`).
- SSH user: `debugger`; private key: `debug-lab/.local/id_ed25519`.
- Root SSH password login is enabled at the user's request. Its password is
  stored in ignored `debug-lab/.local/root_password` and applied on startup;
  do not commit this credential or include it in the image.
- Reserve host port `2222` for other containers; do not use it for this lab.
- Keep `debug-lab/lab.sh`, the CLion settings installer, and the lab README
  consistent with the Compose SSH mapping.
- Run `./debug-lab/lab.sh up` to apply Compose changes, then
  `./debug-lab/lab.sh start` and `./debug-lab/lab.sh verify`.
- Build artifacts persist in the Compose build volume across container recreation.
- Ccache is installed in the lab image and used through CMake's C/C++ compiler
  launchers. Keep `ENABLE_SCCACHE=OFF`. The shared cache is
  `/workspace/build/ccache` in the persistent build volume, with a permanent
  `max_size = 10G` limit. Image configuration is `debug-lab/ccache.conf`;
  the entrypoint reapplies the limit and shared-directory permissions at startup.
- The user prefers CLion to control configuration, compilation, and debugging
  through a **Remote Host (SSH)** toolchain connected to `127.0.0.1:33333`.
- Use the `Mooncake Docker` remote toolchain with the `Mooncake Debug` CMake
  profile, remote build directory `/workspace/build`, and two build jobs.
- Keep **Before launch → Build** enabled for the Mooncake run configurations;
  `lab.sh build` is an optional terminal alternative, not a required IDE step.

### Docker Compose Configuration

The configuration in `debug-lab/compose.yaml` is shown below. Relative paths
are resolved from `debug-lab/`.

```yaml
name: mooncake-debug-lab
services:
  lab:
    container_name: mooncake-debug
    image: mooncake-debug:arm64
    platform: linux/arm64
    build:
      context: .
    init: true
    ports:
      - "127.0.0.1:33333:22"
    cap_add:
      - SYS_PTRACE
    security_opt:
      - seccomp=unconfined
    mem_limit: 8g
    shm_size: 256m
    volumes:
      - ..:/workspace/mooncake
      - build:/workspace/build
      - ./.local/id_ed25519.pub:/run/lab-key.pub:ro
      - ./.local/root_password:/run/lab-root-password:ro
    working_dir: /workspace/mooncake
volumes:
  build:
```

## UBS Memory Transport (`ubsmem`) Copy Threads

- Protocol `ubsmem` copies payloads with CPU `memcpy` through UBS Memory
  shared mappings. Full guide: `mooncake-transfer-engine/README_UBSMEM.md`.
- `MC_UBSMEM_WORKERS` sets the number of copy threads per process (1 to 64,
  default 4). `MC_UBSMEM_CHUNK_SIZE` sets the bytes per chunk (a multiple of
  the page size, default 64 MiB). Each request splits at multiples of the chunk
  size within the UBS object, so chunks never share a page.
- Set both in the environment of every copying process before it starts: the
  producer, the consumer, and the provider when it runs Put or Get on its own
  pool. The transport reads them only once, in the `UbsMemoryTransport`
  constructor. Invalid values make transport creation fail.
- One request uses at most one thread per chunk. Many requests in a batch use
  all threads. Lower the chunk size to split medium objects; each chunk costs
  one ownership acquire/release pair in `cache` mode.
- Use `MC_UBSMEM_WORKERS=1` for single-copy debugging, or if concurrent
  `ubsmem_shmem_set_ownership` calls prove unsafe on hardware.
- Copies on disjoint pages of a pool run in parallel. Copies that overlap
  within one process wait for each other in `UbsMemoryArena::copy`, because a
  release revokes access to its pages. Do not remove this exclusion.
- Speed and the best values are unmeasured. The mock cannot show UB
  bandwidth; tune on a UB host.
