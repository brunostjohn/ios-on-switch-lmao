"""Member pins the catalog needs that ipsw/pins.bzl doesn't carry yet, by kind and role, under the names pins.bzl would
give them. catalog.bzl merges each into its pins.bzl member and refuses a key both define, so moving one into pins.bzl
means deleting it here.

  aea_raw_size     an encrypted archive's decrypted size (its root header's raw size): what content_sha256 covers
  container        how a disk image member holds its APFS container: raw (the bare container) or udif
  container_size   the container's size and SHA-256 once unwrapped (udif only: a raw member's are its content's)
  container_sha256
  payload_size     an IM4P's decoded payload: the bytes switch_img4 returns, for the kernelcache its arm64 slice
  payload_sha256   (the kernelcache's is pins.bzl's derived kernelcache_sha256)

Each value is checked by a test against //ipsw's own outputs or the member's bytes (install/catalog/README.md).
"""

STAGED = {
    "21H461": {
        "app_cryptex": {"container": "raw"},
        "app_cryptex_trust_cache": {
            "payload_sha256": "9b38e7599a211cf892573e5157969e07b0830c629f8db2ca75102c421e5c6a29",
            "payload_size": 744,
        },
        "device_tree": {
            "payload_sha256": "624de9212e4b96085b9f50151a5a162265dcaa9e2d5aeb4e230cbd348d369176",
            "payload_size": 172392,
        },
        "kernelcache": {"payload_size": 46415872},
        "os": {
            "container": "udif",
            "container_sha256": "d8808bea18b949142b3e47bbcb780c3f22faaacd9b40a88f77c23557181edc9c",
            "container_size": 6077546496,
        },
        "static_trust_cache": {
            "payload_sha256": "da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2",
            "payload_size": 59496,
        },
        "system_cryptex": {"container": "raw"},
        "system_cryptex_trust_cache": {
            "payload_sha256": "d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651",
            "payload_size": 1848,
        },
    },
    "24B119": {
        "app_cryptex": {"container": "raw"},
        "app_cryptex_trust_cache": {
            "payload_sha256": "d57f1b1e79a4db9214507d14368a9377a62c293eb80e7d9b3b74060466856c3a",
            "payload_size": 840,
        },
        "device_tree": {
            "payload_sha256": "31cc73dcce9c6e68887ecf10b39986a2deabf13969149125ce3c52ddc06101f7",
            "payload_size": 201372,
        },
        "kernelcache": {"payload_size": 66715648},
        "os": {
            "aea_raw_size": 9065988096,
            "container": "raw",
        },
        "static_trust_cache": {
            "payload_sha256": "a422b50d0cbbd7fc448f6d21f1191d0f31a66da5b5ef710b7938d29311169531",
            "payload_size": 84576,
        },
        "system_cryptex": {
            "aea_raw_size": 6190792704,
            "container": "raw",
        },
        "system_cryptex_trust_cache": {
            "payload_sha256": "1cf081758fcca529446c4b4e8b65faae1e1645bfbbfd1d11d3b94fe38b25d043",
            "payload_size": 3168,
        },
    },
}
