// Copyright (c) 2013, Cloudera, inc.
#ifndef KUDU_TSERVER_TABLET_SERVER_H
#define KUDU_TSERVER_TABLET_SERVER_H

#include <string>
#include <vector>

#include "gutil/gscoped_ptr.h"
#include "gutil/macros.h"
#include "server/metadata.pb.h"
#include "server/webserver_options.h"
#include "server/server_base.h"
#include "tserver/tablet_server_options.h"
#include "tserver/tserver.pb.h"
#include "util/metrics.h"
#include "util/net/net_util.h"
#include "util/net/sockaddr.h"
#include "util/status.h"

namespace kudu {

class FsManager;
class Webserver;

namespace rpc {
class Messenger;
class ServicePool;
}

namespace tserver {

class Heartbeater;
class MutatorManager;
class ScannerManager;
class TSTabletManager;

class TabletServer : public server::ServerBase {
 public:
  // TODO: move this out of this header, since clients want to use this
  // constant as well.
  static const uint16_t kDefaultPort = 7050;
  static const uint16_t kDefaultWebPort = 8050;

  explicit TabletServer(const TabletServerOptions& opts);
  ~TabletServer();

  Status Init();
  Status Start();
  Status Shutdown();

  string ToString() const;

  TSTabletManager* tablet_manager() { return tablet_manager_.get(); }

  ScannerManager* scanner_manager() { return scanner_manager_.get(); }
  const ScannerManager* scanner_manager() const { return scanner_manager_.get(); }

  FsManager* fs_manager() { return fs_manager_.get(); }

  // Returns tablet server metric context. Primarily for use by unit tests.
  const MetricContext& GetMetricContextForTests() const { return metric_ctx_; }

 private:
  friend class TabletServerTest;

  Status ValidateMasterAddressResolution() const;

  bool initted_;

  // The options passed at construction time.
  const TabletServerOptions opts_;

  const MetricContext metric_ctx_;

  gscoped_ptr<FsManager> fs_manager_;

  // Manager for tablets which are available on this server.
  gscoped_ptr<TSTabletManager> tablet_manager_;

  // Manager for open scanners from clients.
  // This is always non-NULL. It is scoped only to minimize header
  // dependencies.
  gscoped_ptr<ScannerManager> scanner_manager_;

  // Thread responsible for heartbeating to the master.
  gscoped_ptr<Heartbeater> heartbeater_;

  DISALLOW_COPY_AND_ASSIGN(TabletServer);
};

} // namespace tserver
} // namespace kudu
#endif
