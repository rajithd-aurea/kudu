// Copyright (c) 2014, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
//
// This module is internal to the client and not a public API.

#include "kudu/master/master_rpc.h"

#include <boost/bind.hpp>
#include <boost/foreach.hpp>

#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/gutil/bind.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/master.proxy.h"
#include "kudu/util/net/net_util.h"


using std::tr1::shared_ptr;
using std::string;
using std::vector;

using kudu::metadata::QuorumPeerPB;
using kudu::rpc::Messenger;
using kudu::rpc::Rpc;

namespace kudu {
namespace master {

////////////////////////////////////////////////////////////
// GetMasterRegistrationRpc
////////////////////////////////////////////////////////////

GetMasterRegistrationRpc::GetMasterRegistrationRpc(const StatusCallback& user_cb,
                                                   const Sockaddr& addr,
                                                   const MonoTime& deadline,
                                                   const shared_ptr<Messenger>& messenger,
                                                   ServerEntryPB* out)
    : Rpc(deadline, messenger),
      user_cb_(user_cb),
      addr_(addr),
      out_(DCHECK_NOTNULL(out)) {
}

GetMasterRegistrationRpc::~GetMasterRegistrationRpc() {
}

void GetMasterRegistrationRpc::SendRpc() {
  MasterServiceProxy proxy(retrier().messenger(),
                           addr_);
  GetMasterRegistrationRequestPB req;
  proxy.GetMasterRegistrationAsync(req, &resp_,
                                   &retrier().controller(),
                                   boost::bind(&GetMasterRegistrationRpc::SendRpcCb,
                                               this,
                                               Status::OK()));
}

string GetMasterRegistrationRpc::ToString() const {
  return strings::Substitute("GetMasterRegistrationRpc(address=$0)",
                             addr_.ToString());
}

void GetMasterRegistrationRpc::SendRpcCb(const Status& status) {
  gscoped_ptr<GetMasterRegistrationRpc> deleter(this);
  Status new_status = status;
  if (new_status.ok() && retrier().HandleResponse(this, &new_status)) {
    ignore_result(deleter.release());
    return;
  }
  if (new_status.ok()) {
    out_->mutable_instance_id()->CopyFrom(resp_.instance_id());
    if (resp_.has_error()) {
      out_->mutable_error()->CopyFrom(resp_.error());
      new_status = StatusFromPB(resp_.error());
    } else {
      out_->mutable_registration()->CopyFrom(resp_.registration());
      out_->set_role(resp_.role());
    }
  }
  user_cb_.Run(new_status);
}

////////////////////////////////////////////////////////////
// GetLeaderMasterRpc
////////////////////////////////////////////////////////////

GetLeaderMasterRpc::GetLeaderMasterRpc(const StatusCallback& user_cb,
                                       const vector<Sockaddr>& addrs,
                                       const MonoTime& deadline,
                                       const shared_ptr<Messenger>& messenger,
                                       HostPort* leader_master)
    : Rpc(deadline, messenger),
      user_cb_(user_cb),
      addrs_(addrs),
      leader_master_(DCHECK_NOTNULL(leader_master)),
      pending_responses_(0),
      completed_(false) {
  // Using resize instead of reserve to explicitly initialized the
  // values.
  responses_.resize(addrs_.size());
}

GetLeaderMasterRpc::~GetLeaderMasterRpc() {
}

string GetLeaderMasterRpc::ToString() const {
  vector<string> sockaddr_str;
  BOOST_FOREACH(const Sockaddr& addr, addrs_) {
    sockaddr_str.push_back(addr.ToString());
  }
  return strings::Substitute("GetLeaderMasterRpc(addrs=$0)",
                             JoinStrings(sockaddr_str, ","));
}

void GetLeaderMasterRpc::SendRpc() {
  for (int i = 0; i < addrs_.size(); i++) {
    lock_guard<simple_spinlock> l(&lock_);
    if (completed_) {
      return;
    }
    GetMasterRegistrationRpc* rpc = new GetMasterRegistrationRpc(
        Bind(&GetLeaderMasterRpc::GetMasterRegistrationRpcCbForNode,
             this, responses_[i]),
        addrs_[i],
        retrier().deadline(),
        retrier().messenger(),
        &responses_[i]);
    rpc->SendRpc();
    ++pending_responses_;
  }
}

void GetLeaderMasterRpc::SendRpcCb(const Status& status) {
  // If we've received replies from all of the nodes without finding
  // the leader, or if there were network errors talking to all of the
  // nodes the error is retriable and we can perform a delayed retry.
  if (status.IsNetworkError() || status.IsNotFound()) {
    retrier().DelayedRetry(this);
    return;
  }
  {
    lock_guard<simple_spinlock> l(&lock_);
    // 'completed_' prevents 'user_cb_' from being invoked twice.
    if (completed_) {
      return;
    }
    completed_ = true;
  }
  user_cb_.Run(status);
}

void GetLeaderMasterRpc::GetMasterRegistrationRpcCbForNode(const ServerEntryPB& resp,
                                                           const Status& status) {
  // TODO: handle the situation where one Master is partitioned from
  // the rest of the Master quorum, all are reachable by the client,
  // and the partitioned node "thinks" it's the leader.
  //
  // The proper way to do so is to add term/index to the responses
  // from the Master, wait for majority of the Masters to respond, and
  // pick the one with the highest term/index as the leader.
  Status new_status = status;
  {
    lock_guard<simple_spinlock> lock(&lock_);
    if (completed_) {
      // If 'user_cb_' has been invoked (see SendRpcCb above), we can
      // stop.
      return;
    }
    if (new_status.ok()) {
      if (resp.role() != QuorumPeerPB::LEADER) {
        // Use a Status::NotFound() to indicate that the node is not
        // the leader: this way, we can handle the case where we've
        // received a reply from all of the nodes in the cluster (no
        // network or other errors encountered), but haven't found a
        // leader (which means that SendRpcCb() above can perform a
        // delayed retry).
        new_status = Status::NotFound("no leader found: " + ToString());
      } else {
        // We've found a leader.
        new_status = HostPortFromPB(
            resp.registration().rpc_addresses(0),
            leader_master_);
      }
    }
    --pending_responses_;
    if (!new_status.ok()) {
      if (pending_responses_ > 0) {
        // Don't call SendRpcCb() on error unless we're the last
        // outstanding response: calling SendRpcCb() will trigger
        // a delayed re-try, which don't need to do unless we've
        // been unable to find a leader so far.
        return;
      }
    }
  }
  // Called if the leader has been determined, or if we've received
  // all of the responses.
  SendRpcCb(new_status);
}


} // namespace master
} // namespace kudu