local host = import 'host.libsonnet';
local allow = { allow: {} };
local storage = { grpc: { client: { address: '127.0.0.1:8980' } } };

{
  clientGrpcServers: [{ listenAddresses: ['127.0.0.1:8982'], authenticationPolicy: allow }],
  workerGrpcServers: [{
    listenAddresses: [a + ':8983' for a in host.listenAddresses],
    authenticationPolicy: allow,
  }],
  buildQueueStateGrpcServers: [{ listenAddresses: ['127.0.0.1:8984'], authenticationPolicy: allow }],
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9982'], authenticationPolicy: allow }],
      enablePrometheus: true,
    },
  },
  contentAddressableStorage: {
    chunkStorage: storage,
    chunkMappingStorage: { chunkMappingValidating: { backend: storage } },
    cdcParameterCache: { cacheSize: 1000, cacheDuration: '60s', cacheReplacementPolicy: 'LEAST_RECENTLY_USED' },
  },
  executeAuthorizer: allow,
  modifyDrainsAuthorizer: allow,
  killOperationsAuthorizer: allow,
  synchronizeAuthorizer: allow,
  actionRouter: {
    simple: {
      platformKeyExtractor: { action: {} },
      invocationKeyExtractors: [{ correlatedInvocationsId: {} }, { toolInvocationId: {} }],
      initialSizeClassAnalyzer: { defaultExecutionTimeout: '3600s', maximumExecutionTimeout: '7200s' },
    },
  },
  platformQueueWithNoWorkersTimeout: '120s',
}
