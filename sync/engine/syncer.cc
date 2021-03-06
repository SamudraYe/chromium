// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sync/engine/syncer.h"

#include "base/debug/trace_event.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/message_loop.h"
#include "base/time.h"
#include "build/build_config.h"
#include "sync/engine/apply_updates_command.h"
#include "sync/engine/build_commit_command.h"
#include "sync/engine/cleanup_disabled_types_command.h"
#include "sync/engine/clear_data_command.h"
#include "sync/engine/conflict_resolver.h"
#include "sync/engine/download_updates_command.h"
#include "sync/engine/get_commit_ids_command.h"
#include "sync/engine/net/server_connection_manager.h"
#include "sync/engine/post_commit_message_command.h"
#include "sync/engine/process_commit_response_command.h"
#include "sync/engine/process_updates_command.h"
#include "sync/engine/resolve_conflicts_command.h"
#include "sync/engine/store_timestamps_command.h"
#include "sync/engine/syncer_types.h"
#include "sync/engine/syncproto.h"
#include "sync/engine/verify_updates_command.h"
#include "sync/syncable/syncable-inl.h"
#include "sync/syncable/syncable.h"

using base::Time;
using base::TimeDelta;
using sync_pb::ClientCommand;
using syncable::Blob;
using syncable::IS_UNAPPLIED_UPDATE;
using syncable::SERVER_CTIME;
using syncable::SERVER_IS_DEL;
using syncable::SERVER_IS_DIR;
using syncable::SERVER_MTIME;
using syncable::SERVER_NON_UNIQUE_NAME;
using syncable::SERVER_PARENT_ID;
using syncable::SERVER_POSITION_IN_PARENT;
using syncable::SERVER_SPECIFICS;
using syncable::SERVER_VERSION;
using syncable::SYNCER;
using syncable::WriteTransaction;

namespace browser_sync {

using sessions::ScopedSessionContextConflictResolver;
using sessions::StatusController;
using sessions::SyncSession;
using sessions::ConflictProgress;

#define ENUM_CASE(x) case x: return #x
const char* SyncerStepToString(const SyncerStep step)
{
  switch (step) {
    ENUM_CASE(SYNCER_BEGIN);
    ENUM_CASE(CLEANUP_DISABLED_TYPES);
    ENUM_CASE(DOWNLOAD_UPDATES);
    ENUM_CASE(PROCESS_CLIENT_COMMAND);
    ENUM_CASE(VERIFY_UPDATES);
    ENUM_CASE(PROCESS_UPDATES);
    ENUM_CASE(STORE_TIMESTAMPS);
    ENUM_CASE(APPLY_UPDATES);
    ENUM_CASE(BUILD_COMMIT_REQUEST);
    ENUM_CASE(POST_COMMIT_MESSAGE);
    ENUM_CASE(PROCESS_COMMIT_RESPONSE);
    ENUM_CASE(RESOLVE_CONFLICTS);
    ENUM_CASE(APPLY_UPDATES_TO_RESOLVE_CONFLICTS);
    ENUM_CASE(CLEAR_PRIVATE_DATA);
    ENUM_CASE(SYNCER_END);
  }
  NOTREACHED();
  return "";
}
#undef ENUM_CASE

Syncer::Syncer()
    : early_exit_requested_(false) {
}

Syncer::~Syncer() {}

bool Syncer::ExitRequested() {
  base::AutoLock lock(early_exit_requested_lock_);
  return early_exit_requested_;
}

void Syncer::RequestEarlyExit() {
  base::AutoLock lock(early_exit_requested_lock_);
  early_exit_requested_ = true;
}

void Syncer::SyncShare(sessions::SyncSession* session,
                       SyncerStep first_step,
                       SyncerStep last_step) {
  ScopedSessionContextConflictResolver scoped(session->context(),
                                              &resolver_);
  session->mutable_status_controller()->UpdateStartTime();
  SyncerStep current_step = first_step;

  SyncerStep next_step = current_step;
  while (!ExitRequested()) {
    TRACE_EVENT1("sync", "SyncerStateMachine",
                 "state", SyncerStepToString(current_step));
    DVLOG(1) << "Syncer step:" << SyncerStepToString(current_step);

    switch (current_step) {
      case SYNCER_BEGIN:
        session->context()->PruneUnthrottledTypes(base::TimeTicks::Now());
        session->SendEventNotification(SyncEngineEvent::SYNC_CYCLE_BEGIN);

        next_step = CLEANUP_DISABLED_TYPES;
        break;
      case CLEANUP_DISABLED_TYPES: {
        CleanupDisabledTypesCommand cleanup;
        cleanup.Execute(session);
        next_step = DOWNLOAD_UPDATES;
        break;
      }
      case DOWNLOAD_UPDATES: {
        // TODO(akalin): We may want to propagate this switch up
        // eventually.
#if defined(OS_ANDROID)
        const bool kCreateMobileBookmarksFolder = true;
#else
        const bool kCreateMobileBookmarksFolder = false;
#endif
        DownloadUpdatesCommand download_updates(kCreateMobileBookmarksFolder);
        session->mutable_status_controller()->set_last_download_updates_result(
            download_updates.Execute(session));
        next_step = PROCESS_CLIENT_COMMAND;
        break;
      }
      case PROCESS_CLIENT_COMMAND: {
        ProcessClientCommand(session);
        next_step = VERIFY_UPDATES;
        break;
      }
      case VERIFY_UPDATES: {
        VerifyUpdatesCommand verify_updates;
        verify_updates.Execute(session);
        next_step = PROCESS_UPDATES;
        break;
      }
      case PROCESS_UPDATES: {
        ProcessUpdatesCommand process_updates;
        process_updates.Execute(session);
        next_step = STORE_TIMESTAMPS;
        break;
      }
      case STORE_TIMESTAMPS: {
        StoreTimestampsCommand store_timestamps;
        store_timestamps.Execute(session);
        // We download all of the updates before attempting to apply them.
        if (!session->status_controller().download_updates_succeeded()) {
          // We may have downloaded some updates, but if the latest download
          // attempt failed then we don't have all the updates.  We'll leave
          // it to a retry job to pick up where we left off.
          last_step = SYNCER_END; // Necessary for CONFIGURATION mode.
          next_step = SYNCER_END;
          DVLOG(1) << "Aborting sync cycle due to download updates failure";
        } else if (!session->status_controller()
                       .ServerSaysNothingMoreToDownload()) {
          next_step = DOWNLOAD_UPDATES;
        } else {
          next_step = APPLY_UPDATES;
        }
        break;
      }
      case APPLY_UPDATES: {
        ApplyUpdatesCommand apply_updates;
        apply_updates.Execute(session);
        if (last_step == APPLY_UPDATES) {
          // We're in configuration mode, but we still need to run the
          // SYNCER_END step.
          last_step = SYNCER_END;
          next_step = SYNCER_END;
        } else {
          next_step = BUILD_COMMIT_REQUEST;
        }
        break;
      }
      // These two steps are combined since they are executed within the same
      // write transaction.
      case BUILD_COMMIT_REQUEST: {
        syncable::Directory* dir = session->context()->directory();
        WriteTransaction trans(FROM_HERE, SYNCER, dir);
        sessions::ScopedSetSessionWriteTransaction set_trans(session, &trans);

        DVLOG(1) << "Getting the Commit IDs";
        GetCommitIdsCommand get_commit_ids_command(
            session->context()->max_commit_batch_size());
        get_commit_ids_command.Execute(session);

        if (!session->status_controller().commit_ids().empty()) {
          DVLOG(1) << "Building a commit message";

          // This isn't perfect, since the set of extensions activity may not
          // correlate exactly with the items being committed.  That's OK as
          // long as we're looking for a rough estimate of extensions activity,
          // not an precise mapping of which commits were triggered by which
          // extension.
          //
          // We will push this list of extensions activity back into the
          // ExtensionsActivityMonitor if this commit turns out to not contain
          // any bookmarks, or if the commit fails.
          session->context()->extensions_monitor()->GetAndClearRecords(
              session->mutable_extensions_activity());

          BuildCommitCommand build_commit_command;
          build_commit_command.Execute(session);

          next_step = POST_COMMIT_MESSAGE;
        } else {
          next_step = RESOLVE_CONFLICTS;
        }

        break;
      }
      case POST_COMMIT_MESSAGE: {
        PostCommitMessageCommand post_commit_command;
        session->mutable_status_controller()->set_last_post_commit_result(
            post_commit_command.Execute(session));
        next_step = PROCESS_COMMIT_RESPONSE;
        break;
      }
      case PROCESS_COMMIT_RESPONSE: {
        ProcessCommitResponseCommand process_response_command;
        session->mutable_status_controller()->
            set_last_process_commit_response_result(
                process_response_command.Execute(session));
        next_step = RESOLVE_CONFLICTS;
        break;
      }
      case RESOLVE_CONFLICTS: {
        StatusController* status = session->mutable_status_controller();
        status->reset_conflicts_resolved();
        ResolveConflictsCommand resolve_conflicts_command;
        resolve_conflicts_command.Execute(session);

        // Has ConflictingUpdates includes both resolvable and unresolvable
        // conflicts. If we have either, we want to attempt to reapply.
        if (status->HasConflictingUpdates())
          next_step = APPLY_UPDATES_TO_RESOLVE_CONFLICTS;
        else
          next_step = SYNCER_END;
        break;
      }
      case APPLY_UPDATES_TO_RESOLVE_CONFLICTS: {
        StatusController* status = session->mutable_status_controller();
        DVLOG(1) << "Applying updates to resolve conflicts";
        ApplyUpdatesCommand apply_updates;

        // We only care to resolve conflicts again if we made progress on the
        // simple conflicts.
        int before_blocking_conflicting_updates =
            status->TotalNumSimpleConflictingItems();
        apply_updates.Execute(session);
        int after_blocking_conflicting_updates =
            status->TotalNumSimpleConflictingItems();
        // If the following call sets the conflicts_resolved value to true,
        // SyncSession::HasMoreToSync() will send us into another sync cycle
        // after this one completes.
        //
        // TODO(rlarocque, 109072): Make conflict resolution not require
        // extra sync cycles/GetUpdates.
        status->update_conflicts_resolved(before_blocking_conflicting_updates >
                                          after_blocking_conflicting_updates);
        next_step = SYNCER_END;
        break;
      }
      case CLEAR_PRIVATE_DATA: {
        ClearDataCommand clear_data_command;
        clear_data_command.Execute(session);
        next_step = SYNCER_END;
        break;
      }
      case SYNCER_END: {
        session->SendEventNotification(SyncEngineEvent::SYNC_CYCLE_ENDED);
        next_step = SYNCER_END;
        break;
      }
      default:
        LOG(ERROR) << "Unknown command: " << current_step;
    }
    DVLOG(2) << "last step: " << SyncerStepToString(last_step) << ", "
             << "current step: " << SyncerStepToString(current_step) << ", "
             << "next step: " << SyncerStepToString(next_step) << ", "
             << "snapshot: " << session->TakeSnapshot().ToString();
    if (last_step == current_step)
      break;
    current_step = next_step;
  }
}

void Syncer::ProcessClientCommand(sessions::SyncSession* session) {
  const ClientToServerResponse& response =
      session->status_controller().updates_response();
  if (!response.has_client_command())
    return;
  const ClientCommand& command = response.client_command();

  // The server limits the number of items a client can commit in one batch.
  if (command.has_max_commit_batch_size()) {
    session->context()->set_max_commit_batch_size(
        command.max_commit_batch_size());
  }
  if (command.has_set_sync_long_poll_interval()) {
    session->delegate()->OnReceivedLongPollIntervalUpdate(
        TimeDelta::FromSeconds(command.set_sync_long_poll_interval()));
  }
  if (command.has_set_sync_poll_interval()) {
    session->delegate()->OnReceivedShortPollIntervalUpdate(
        TimeDelta::FromSeconds(command.set_sync_poll_interval()));
  }

  if (command.has_sessions_commit_delay_seconds()) {
    session->delegate()->OnReceivedSessionsCommitDelay(
        TimeDelta::FromSeconds(command.sessions_commit_delay_seconds()));
  }
}

void CopyServerFields(syncable::Entry* src, syncable::MutableEntry* dest) {
  dest->Put(SERVER_NON_UNIQUE_NAME, src->Get(SERVER_NON_UNIQUE_NAME));
  dest->Put(SERVER_PARENT_ID, src->Get(SERVER_PARENT_ID));
  dest->Put(SERVER_MTIME, src->Get(SERVER_MTIME));
  dest->Put(SERVER_CTIME, src->Get(SERVER_CTIME));
  dest->Put(SERVER_VERSION, src->Get(SERVER_VERSION));
  dest->Put(SERVER_IS_DIR, src->Get(SERVER_IS_DIR));
  dest->Put(SERVER_IS_DEL, src->Get(SERVER_IS_DEL));
  dest->Put(IS_UNAPPLIED_UPDATE, src->Get(IS_UNAPPLIED_UPDATE));
  dest->Put(SERVER_SPECIFICS, src->Get(SERVER_SPECIFICS));
  dest->Put(SERVER_POSITION_IN_PARENT, src->Get(SERVER_POSITION_IN_PARENT));
}

void ClearServerData(syncable::MutableEntry* entry) {
  entry->Put(SERVER_NON_UNIQUE_NAME, "");
  entry->Put(SERVER_PARENT_ID, syncable::GetNullId());
  entry->Put(SERVER_MTIME, Time());
  entry->Put(SERVER_CTIME, Time());
  entry->Put(SERVER_VERSION, 0);
  entry->Put(SERVER_IS_DIR, false);
  entry->Put(SERVER_IS_DEL, false);
  entry->Put(IS_UNAPPLIED_UPDATE, false);
  entry->Put(SERVER_SPECIFICS, sync_pb::EntitySpecifics::default_instance());
  entry->Put(SERVER_POSITION_IN_PARENT, 0);
}

}  // namespace browser_sync
