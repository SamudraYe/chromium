// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_LOW_MEMORY_OBSERVER_H_
#define CHROME_BROWSER_LOW_MEMORY_OBSERVER_H_
#pragma once

#include "base/memory/ref_counted.h"

#if !defined(OS_CHROMEOS)
#error This file only meant to be compiled on ChromeOS
#endif

namespace browser {

class LowMemoryObserverImpl;

////////////////////////////////////////////////////////////////////////////////
// LowMemoryObserver
//
// Class to handle observation of low memory device for changes so that we
// can get a signal from the kernel about low memory conditions and discard tabs
// when that happens, instead of waiting for the OOM killer to kill renderers
// (which is more drastic, but still necessary and possible).
//
// This object starts and stops the observation, and can be created or deleted
// from any thread, but the observation occurs on the FILE thread, and tabs are
// discarded on the UI thread.
class LowMemoryObserver {
 public:
  LowMemoryObserver();
  ~LowMemoryObserver();

  void Start();
  void Stop();
 private:
  scoped_refptr<LowMemoryObserverImpl> observer_;

  DISALLOW_COPY_AND_ASSIGN(LowMemoryObserver);
};

}  // namespace browser

#endif  // CHROME_BROWSER_LOW_MEMORY_OBSERVER_H_
