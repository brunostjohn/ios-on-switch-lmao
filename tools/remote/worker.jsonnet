// bb_worker on a Mac: takes darwin actions from the scheduler and runs them through bb_runner in a build directory it
// serves itself over NFSv4 (macOS mounts it for the user, no root): an action's inputs appear without being copied,
// and only the files it reads are fetched, into a capped local CAS. Every host value comes from the LaunchAgent's
// environment (install.sh); the two tokens sit beside this file, chmod 600.
local dir = std.extVar('SWITCH_WORKER_DIR');
local cacheGiB = std.parseInt(std.extVar('SWITCH_WORKER_CACHE_GIB'));
local cacheToken = std.stripChars(importstr 'cache.token', '\n');
local workerToken = std.stripChars(importstr 'worker.token', '\n');
local frontend = {
  grpc: {
    client: {
      address: std.extVar('SWITCH_WORKER_FRONTEND'),
      addMetadata: [{ header: 'authorization', values: ['Bearer ' + cacheToken] }],
    },
    // On the LAN, zstd only costs the worker and the frontend CPU.
    enableCompression: false,
  },
};
local lru = { cacheReplacementPolicy: 'LEAST_RECENTLY_USED' };

{
  blobstore: {
    contentAddressableStorage: {
      chunkStorage: {
        readCaching: {
          slow: frontend,
          fast: {
            'local': {
              keyLocationMap: {
                onBlockDevice: { file: { path: dir + '/cas/key_location_map', sizeBytes: 256 * 1024 * 1024 } },
                maximumGetAttempts: 16,
                maximumPutAttempts: 64,
              },
              oldBlocks: 8,
              currentBlocks: 24,
              newBlocks: 3,
              // The worker's whole lasting disk use: a sparse file that grows to this size.
              blocksOnBlockDevice: {
                source: { file: { path: dir + '/cas/blocks', sizeBytes: cacheGiB * 1024 * 1024 * 1024 } },
                spareBlocks: 3,
              },
              persistent: { stateDirectoryPath: dir + '/cas/persistent_state', minimumEpochInterval: '300s' },
            },
          },
          replicator: { deduplicating: { 'local': {} } },
        },
      },
      chunkMappingStorage: { chunkMappingValidating: { backend: frontend } },
      cdcParameterCache: lru { cacheSize: 1000, cacheDuration: '60s' },
      // A virtual build directory reads the same blobs again at random offsets.
      chunkMappingCache: lru { cacheSize: 100000, cacheDuration: '600s' },
      chunkCache: lru { cacheSize: 10000, cacheDuration: '60s' },
    },
    actionCache: frontend,
  },
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  scheduler: {
    address: std.extVar('SWITCH_WORKER_SCHEDULER'),
    addMetadata: [{ header: 'authorization', values: ['Bearer ' + workerToken] }],
  },
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9986'], authenticationPolicy: { allow: {} } }],
      enablePrometheus: true,
    },
  },
  buildDirectories: [{
    virtual: {
      mount: {
        mountPath: dir + '/build',
        nfsv4: {
          darwin: { socketPath: dir + '/nfs.sock' },
          enforcedLeaseTime: '120s',
          announcedLeaseTime: '60s',
        },
      },
      maximumExecutionTimeoutCompensation: '3600s',
      maximumWritableFileUploadDelay: '60s',
    },
    runners: [{
      endpoint: { address: 'unix://' + dir + '/runner.sock' },
      concurrency: std.parseInt(std.extVar('SWITCH_WORKER_CONCURRENCY')),
      platform: {
        // Bazel's exec platform //bazel/platforms:exec_darwin_arm64, property for property.
        properties: [
          { name: 'ISA', value: 'arm64' },
          { name: 'OSFamily', value: 'darwin' },
          { name: 'xcode', value: std.extVar('SWITCH_WORKER_XCODE_BUILD') },
        ],
      },
      maximumFilePoolFileCount: 1000000,
      maximumFilePoolSizeBytes: 8 * 1024 * 1024 * 1024,
      workerId: { host: std.extVar('SWITCH_WORKER_HOST') },
    }],
  }],
  // What actions write lives here until it is uploaded.
  filePool: { blockDevice: { file: { path: dir + '/filepool', sizeBytes: 8 * 1024 * 1024 * 1024 } } },
  inputDownloadConcurrency: 128,
  outputUploadConcurrency: 64,
  directoryCache: lru { maximumCount: 1000000, maximumSizeBytes: 1024 * 1024 * 1024 },
}
