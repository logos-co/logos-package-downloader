# Index addresses

A repository can publish its index on Logos Storage and over HTTP. The client
gets the index the same way it downloads a package: it follows the download
source, and it falls back from Logos Storage to HTTP.

## Addresses

- `logos-repo.json` can list the addresses of its index in `indexUrls`.
- An address is either `logos:<network>:<key>` or an HTTP URL.
- In a `logos:` address, the key is a dataset CID or a name key.
- A name key is resolved to a dataset CID first. The client then downloads
  this CID.
- `indexUrl` stays in `logos-repo.json`, and it is still required. Older
  clients read only `indexUrl`.
- When `indexUrls` is not a list, the client ignores it.

## Which address the client uses

- The storage address is the `logos:` address of `indexUrls` on the storage
  node's network.
- The HTTP address is the HTTP URL of `indexUrls`. When `indexUrls` has none,
  it is `indexUrl`.

| Download source | Addresses tried |
|-----------------|-----------------|
| `any`   | the storage address, then the HTTP address |
| `logos` | the storage address only: HTTP is never used |
| `http`  | the HTTP address only: the storage node is never asked |

A repository without `indexUrls` gets its index from `indexUrl` over HTTP,
whatever the download source.

## Failures

- A name that doesn't resolve fails: no peer replied, or the name has no
  record. With `any`, the client then tries the HTTP address.
- When every address tried fails, the index fetch fails. The error names each
  address that failed and why.
- With `logos`, when there is no storage address, the index fetch fails.