// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {TestRunner} from 'test_runner';
import {ElementsTestRunner} from 'elements_test_runner';

(async function() {
  TestRunner.addResult(`This test verifies that masonry layouts with direction rtl and ltr are correctly highlighted.\n`);
  await TestRunner.showPanel('elements');
  await TestRunner.loadHTML(`
      <style>
      .masonry {
        display: masonry;
        position: absolute;
        top: 50px;
        left: 10px;
        grid-template-columns: 20px 50px;
        width: 100px;
        height: 100px;
      }
      .ltr-dir {
        direction: ltr;
      }
      .rtl-dir {
        direction: rtl;
      }
      .with-gap {
        grid-gap: 1em;
      }
      </style>

      <div>
          <div class="masonry ltr-dir" id="ltrMasonry">
              <div style="width: 100%; height: 100px; background: burlywood"></div>
              <div style="width: 100%; height: 100px; background: cadetblue"></div>
          </div>
          <div class="masonry rtl-dir" id="rtlMasonry">
              <div style="width: 100%; height: 100px; background: burlywood"></div>
              <div style="width: 100%; height: 100px; background: cadetblue"></div>
          </div>
          <div class="masonry ltr-dir with-gap" id="ltrMasonryGap">
              <div style="width: 100%; height: 100px; background: burlywood"></div>
              <div style="width: 100%; height: 100px; background: cadetblue"></div>
          </div>
          <div class="masonry rtl-dir with-gap" id="rtlMasonryGap">
              <div style="width: 100%; height: 100px; background: burlywood"></div>
              <div style="width: 100%; height: 100px; background: cadetblue"></div>
      </div>
`);
  function dumpGridHighlight(id) {
    return new Promise(resolve => ElementsTestRunner.dumpInspectorHighlightJSON(id, resolve));
  }

  await dumpGridHighlight('ltrMasonry');
  await dumpGridHighlight('rtlMasonry');
  await dumpGridHighlight('ltrMasonryGap');
  await dumpGridHighlight('rtlMasonryGap');

  TestRunner.completeTest();
})();
