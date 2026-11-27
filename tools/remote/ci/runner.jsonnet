local dir = std.extVar('SWITCH_BB_WORKER_DIR');

{
  buildDirectoryPath: dir + '/build',
  grpcServers: [{ listenPaths: [dir + '/runner.sock'], authenticationPolicy: { allow: {} } }],
  global: {},
  setTmpdirEnvironmentVariable: true,
  appleXcodeDeveloperDirectories: {
    [std.extVar('SWITCH_WORKER_XCODE_VERSION')]: std.extVar('SWITCH_WORKER_DEVELOPER_DIR'),
  },
}
