// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/android/media_session.h"

#include "base/android/context_utils.h"
#include "base/android/jni_android.h"
#include "content/browser/media/android/media_session_observer.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "jni/MediaSession_jni.h"

namespace content {

using MediaSessionSuspendedSource =
    MediaSessionUmaHelper::MediaSessionSuspendedSource;

DEFINE_WEB_CONTENTS_USER_DATA_KEY(MediaSession);

MediaSession::PlayerIdentifier::PlayerIdentifier(MediaSessionObserver* observer,
                                                 int player_id)
    : observer(observer),
      player_id(player_id) {
}

bool MediaSession::PlayerIdentifier::operator==(
    const PlayerIdentifier& other) const {
  return this->observer == other.observer && this->player_id == other.player_id;
}

size_t MediaSession::PlayerIdentifier::Hash::operator()(
    const PlayerIdentifier& player_identifier) const {
  size_t hash = BASE_HASH_NAMESPACE::hash<MediaSessionObserver*>()(
      player_identifier.observer);
  hash += BASE_HASH_NAMESPACE::hash<int>()(player_identifier.player_id);
  return hash;
}

// static
bool content::MediaSession::RegisterMediaSession(JNIEnv* env) {
  return RegisterNativesImpl(env);
}

// static
MediaSession* MediaSession::Get(WebContents* web_contents) {
  MediaSession* session = FromWebContents(web_contents);
  if (!session) {
    CreateForWebContents(web_contents);
    session = FromWebContents(web_contents);
    session->Initialize();
  }
  return session;
}

MediaSession::~MediaSession() {
  DCHECK(players_.empty());
  DCHECK(audio_focus_state_ == State::INACTIVE);
}

bool MediaSession::AddPlayer(MediaSessionObserver* observer,
                             int player_id,
                             Type type) {
  // If the audio focus is already granted and is of type Content, there is
  // nothing to do. If it is granted of type Transient the requested type is
  // also transient, there is also nothing to do. Otherwise, the session needs
  // to request audio focus again.
  if (audio_focus_state_ == State::ACTIVE &&
      (audio_focus_type_ == Type::Content || audio_focus_type_ == type)) {
    players_.insert(PlayerIdentifier(observer, player_id));
    return true;
  }

  State old_audio_focus_state = audio_focus_state_;
  State audio_focus_state = RequestSystemAudioFocus(type) ? State::ACTIVE
                                                          : State::INACTIVE;
  SetAudioFocusState(audio_focus_state);
  audio_focus_type_ = type;

  if (audio_focus_state_ != State::ACTIVE)
    return false;

  // The session should be reset if a player is starting while all players are
  // suspended.
  if (old_audio_focus_state != State::ACTIVE)
    players_.clear();

  players_.insert(PlayerIdentifier(observer, player_id));
  UpdateWebContents();

  return true;
}

void MediaSession::RemovePlayer(MediaSessionObserver* observer,
                                int player_id) {
  auto it = players_.find(PlayerIdentifier(observer, player_id));
  if (it != players_.end())
    players_.erase(it);

  AbandonSystemAudioFocusIfNeeded();
}

void MediaSession::RemovePlayers(MediaSessionObserver* observer) {
  for (auto it = players_.begin(); it != players_.end();) {
    if (it->observer == observer)
      players_.erase(it++);
    else
      ++it;
  }

  AbandonSystemAudioFocusIfNeeded();
}

void MediaSession::OnSuspend(JNIEnv* env,
                             const JavaParamRef<jobject>& obj,
                             jboolean temporary) {
  // TODO(mlamouri): this check makes it so that if a MediaSession is paused and
  // then loses audio focus, it will still stay in the Suspended state.
  // See https://crbug.com/539998
  if (audio_focus_state_ != State::ACTIVE)
    return;

  OnSuspendInternal(SuspendType::SYSTEM,
                    temporary ? State::SUSPENDED : State::INACTIVE);

}

void MediaSession::OnResume(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  if (audio_focus_state_ != State::SUSPENDED)
    return;

  OnResumeInternal(SuspendType::SYSTEM);
}

void MediaSession::OnPlayerPaused(MediaSessionObserver* observer,
                                  int player_id) {
  // If a playback is completed, BrowserMediaPlayerManager will call
  // OnPlayerPaused() after RemovePlayer(). This is a workaround.
  // Also, this method may be called when a player that is not added
  // to this session (e.g. a silent video) is paused. MediaSession
  // should ignore the paused player for this case.
  if (!players_.count(PlayerIdentifier(observer, player_id)))
    return;

  // If there is more than one observer, remove the paused one from the session.
  if (players_.size() != 1) {
    RemovePlayer(observer, player_id);
    return;
  }

  // Otherwise, suspend the session.
  DCHECK(!IsSuspended());
  OnSuspendInternal(SuspendType::CONTENT, State::SUSPENDED);
}

void MediaSession::Resume() {
  DCHECK(IsSuspended());

  // Request audio focus again in case we lost it because another app started
  // playing while the playback was paused.
  State audio_focus_state = RequestSystemAudioFocus(audio_focus_type_)
                                ? State::ACTIVE
                                : State::INACTIVE;
  SetAudioFocusState(audio_focus_state);

  if (audio_focus_state_ != State::ACTIVE)
    return;

  OnResumeInternal(SuspendType::UI);
}

void MediaSession::Suspend() {
  DCHECK(!IsSuspended());

  OnSuspendInternal(SuspendType::UI, State::SUSPENDED);
}

void MediaSession::Stop() {
  DCHECK(audio_focus_state_ != State::INACTIVE);

  if (audio_focus_state_ != State::SUSPENDED)
    OnSuspendInternal(SuspendType::UI, State::SUSPENDED);

  DCHECK(audio_focus_state_ == State::SUSPENDED);
  players_.clear();
  AbandonSystemAudioFocusIfNeeded();
}

bool MediaSession::IsSuspended() const {
  // TODO(mlamouri): should be == State::SUSPENDED.
  return audio_focus_state_ != State::ACTIVE;
}

bool MediaSession::IsControllable() const {
  // Only content type media session can be controllable unless it is inactive.
  return audio_focus_state_ != State::INACTIVE &&
         audio_focus_type_ == Type::Content;
}

void MediaSession::ResetJavaRefForTest() {
  j_media_session_.Reset();
}

bool MediaSession::IsActiveForTest() const {
  return audio_focus_state_ == State::ACTIVE;
}

MediaSession::Type MediaSession::audio_focus_type_for_test() const {
  return audio_focus_type_;
}

MediaSessionUmaHelper* MediaSession::uma_helper_for_test() {
  return &uma_helper_;
}

void MediaSession::RemoveAllPlayersForTest() {
  players_.clear();
  AbandonSystemAudioFocusIfNeeded();
}

void MediaSession::OnSuspendInternal(SuspendType type, State new_state) {
  DCHECK(new_state == State::SUSPENDED || new_state == State::INACTIVE);
  // UI suspend cannot use State::INACTIVE.
  DCHECK(type == SuspendType::SYSTEM || new_state == State::SUSPENDED);

  switch (type) {
    case SuspendType::UI:
      uma_helper_.RecordSessionSuspended(MediaSessionSuspendedSource::UI);
      break;
    case SuspendType::SYSTEM:
      switch (new_state) {
        case State::SUSPENDED:
          uma_helper_.RecordSessionSuspended(
              MediaSessionSuspendedSource::SystemTransient);
          break;
        case State::INACTIVE:
          uma_helper_.RecordSessionSuspended(
              MediaSessionSuspendedSource::SystemPermanent);
          break;
        case State::ACTIVE:
          NOTREACHED();
          break;
      }
      break;
    case SuspendType::CONTENT:
      uma_helper_.RecordSessionSuspended(MediaSessionSuspendedSource::CONTENT);
      break;
  }

  SetAudioFocusState(new_state);
  suspend_type_ = type;

  if (type != SuspendType::CONTENT) {
    // SuspendType::CONTENT happens when the suspend action came from
    // the page in which case the player is already paused.
    // Otherwise, the players need to be paused.
    for (const auto& it : players_)
      it.observer->OnSuspend(it.player_id);
  }

  UpdateWebContents();
}

void MediaSession::OnResumeInternal(SuspendType type) {
  if (type == SuspendType::SYSTEM && suspend_type_ != type)
    return;

  SetAudioFocusState(State::ACTIVE);

  for (const auto& it : players_)
    it.observer->OnResume(it.player_id);

  UpdateWebContents();
}

MediaSession::MediaSession(WebContents* web_contents)
    : WebContentsObserver(web_contents),
      audio_focus_state_(State::INACTIVE),
      audio_focus_type_(Type::Transient) {}

void MediaSession::Initialize() {
  JNIEnv* env = base::android::AttachCurrentThread();
  DCHECK(env);
  j_media_session_.Reset(Java_MediaSession_createMediaSession(
      env,
      base::android::GetApplicationContext(),
      reinterpret_cast<intptr_t>(this)));
}

bool MediaSession::RequestSystemAudioFocus(Type type) {
  // During tests, j_media_session_ might be null.
  if (j_media_session_.is_null())
    return true;

  JNIEnv* env = base::android::AttachCurrentThread();
  DCHECK(env);
  bool result = Java_MediaSession_requestAudioFocus(env, j_media_session_.obj(),
                                                    type == Type::Transient);
  uma_helper_.RecordRequestAudioFocusResult(result);
  return result;
}

void MediaSession::AbandonSystemAudioFocusIfNeeded() {
  if (audio_focus_state_ == State::INACTIVE || !players_.empty())
    return;

  // During tests, j_media_session_ might be null.
  if (!j_media_session_.is_null()) {
    JNIEnv* env = base::android::AttachCurrentThread();
    DCHECK(env);
    Java_MediaSession_abandonAudioFocus(env, j_media_session_.obj());
  }

  SetAudioFocusState(State::INACTIVE);
  UpdateWebContents();
}

void MediaSession::UpdateWebContents() {
  static_cast<WebContentsImpl*>(web_contents())->OnMediaSessionStateChanged();
}

void MediaSession::SetAudioFocusState(State audio_focus_state) {
  if (audio_focus_state == audio_focus_state_)
    return;

  audio_focus_state_ = audio_focus_state;
  switch (audio_focus_state_) {
    case State::ACTIVE:
      uma_helper_.OnSessionActive();
      break;
    case State::SUSPENDED:
      uma_helper_.OnSessionSuspended();
      break;
    case State::INACTIVE:
      uma_helper_.OnSessionInactive();
      break;
  }
}

}  // namespace content
