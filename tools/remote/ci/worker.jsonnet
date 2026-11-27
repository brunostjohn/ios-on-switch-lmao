local host = import 'host.libsonnet';
local dir = std.extVar('SWITCH_BB_WORKER_DIR');
local gpu = std.extVar('SWITCH_WORKER_GPU');
local storage = { grpc: { client: { address: host.storageAddress } } };
local platform = [
  { name: 'ISA', value: 'arm64' },
  { name: 'OSFamily', value: 'darwin' },
  { name: 'xcode', value: std.extVar('SWITCH_WORKER_XCODE_BUILD') },
];
local runner(concurrency, properties) = {
  endpoint: { address: 'unix://' + dir + '/runner.sock' },
  concurrency: concurrency,
  platform: { properties: properties },
  maximumFilePoolFileCount: 200000,
  maximumFilePoolSizeBytes: host.filepoolGiB * 1024 * 1024 * 1024,
  workerId: { host: host.name },
};

{
  blobstore: {
    contentAddressableStorage: {
      chunkStorage: storage,
      chunkMappingStorage: { chunkMappingValidating: { backend: storage } },
      cdcParameterCache: { cacheSize: 1000, cacheDuration: '60s', cacheReplacementPolicy: 'LEAST_RECENTLY_USED' },
    },
    actionCache: storage,
  },
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  scheduler: { address: host.schedulerAddress },
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9986'], authenticationPolicy: { allow: {} } }],
      enablePrometheus: true,
    },
  },
  buildDirectories: [{
    native: {
      buildDirectoryPath: dir + '/build',
      cacheDirectoryPath: dir + '/cache',
      maximumCacheFileCount: 1000000,
      maximumCacheSizeBytes: host.workerCacheGiB * 1024 * 1024 * 1024,
      cacheReplacementPolicy: 'LEAST_RECENTLY_USED',
    },
    runners: [runner(host.concurrency, platform)] +
             (if gpu == '' then [] else [runner(1, platform + [{ name: 'gpu', value: gpu }])]),
  }],
  filePool: { blockDevice: { file: { path: dir + '/filepool', sizeBytes: host.filepoolGiB * 1024 * 1024 * 1024 } } },
  inputDownloadConcurrency: 64,
  outputUploadConcurrency: 32,
  directoryCache: {
    maximumCount: 1000000,
    maximumSizeBytes: 512 * 1024 * 1024,
    cacheReplacementPolicy: 'LEAST_RECENTLY_USED',
  },
}
