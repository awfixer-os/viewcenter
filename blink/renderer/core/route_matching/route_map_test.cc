// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/route_matching/route_map.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

namespace blink {

namespace {

class RouteMapTest : public PageTestBase {
 public:
  RouteMap& GetRouteMap() const { return RouteMap::Ensure(GetDocument()); }

  void SetURL(const String& url) {
    GetDocument().SetURL(KURL(url));
    GetRouteMap().UpdateActiveRoutes();
  }
};

TEST_F(RouteMapTest, ParseAndMatch) {
  SetURL("https://example.com/foo");

  RouteMap& route_map = GetRouteMap();
  route_map.ParseAndApplyRoutes(R"({
    "routes": [
      {
        "name": "route1",
        "pattern": "/foo"
      },
      {
        "name": "route2",
        "pattern": ["/bar", "/baz"]
      }
    ]
  })");

  EXPECT_TRUE(route_map.MatchesRoute("route1", RoutePreposition::kAt));
  EXPECT_FALSE(route_map.MatchesRoute("route2", RoutePreposition::kAt));

  SetURL("https://example.com/bar");
  EXPECT_FALSE(route_map.MatchesRoute("route1", RoutePreposition::kAt));
  EXPECT_TRUE(route_map.MatchesRoute("route2", RoutePreposition::kAt));

  SetURL("https://example.com/baz");
  EXPECT_FALSE(route_map.MatchesRoute("route1", RoutePreposition::kAt));
  EXPECT_TRUE(route_map.MatchesRoute("route2", RoutePreposition::kAt));
}

TEST_F(RouteMapTest, GetActiveRoutes) {
  SetURL("https://example.com/foo");

  RouteMap& route_map = GetRouteMap();
  route_map.ParseAndApplyRoutes(R"({
    "routes": [
      {
        "name": "route1",
        "pattern": "/foo"
      },
      {
        "name": "route2",
        "pattern": ["/bar", "/baz"]
      },
      {
        "name": "route3",
        "pattern": "/foo"
      }
    ]
  })");

  RouteMatchState::MatchCollection collection;
  route_map.GetActiveRoutes(RoutePreposition::kAt, &collection);
  EXPECT_EQ(2u, collection.size());

  SetURL("https://example.com/bar");
  route_map.GetActiveRoutes(RoutePreposition::kAt, &collection);
  EXPECT_EQ(1u, collection.size());
}

}  // anonymous namespace

}  // namespace blink
