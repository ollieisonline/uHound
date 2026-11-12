// How media items are presented in the list: display name, the short
// right-aligned tag, and the longer detail line shown for the selection.
// Pure functions over MediaItem so they're covered by host tests.
#pragma once
#include "hound_client.h"
#include "ui/app_ui.h"

#include <string>

// True for things Player can play (video, audio, a Live TV channel)
// rather than folders to browse into.
bool isPlayableItem(const MediaItem& item);

// True for the "Live TV" view, whose contents come from
// HoundClient::getLiveTvChannels() instead of getItems().
bool isLiveTvView(const MediaItem& item);

// Albums and playlists: + shuffles their tracks.
bool isShufflableContainer(const MediaItem& item);

std::string itemDisplayName(const MediaItem& item);
std::string itemTag(const MediaItem& item);
std::string itemDetail(const MediaItem& item);

// Icon shown next to the item in lists.
ui::Icon itemIcon(const MediaItem& item);

// The rows Ufin adds to the top of Home (not libraries on the server).
static const char* const HOME_RESUME = "UfinResume";
static const char* const HOME_NEXT_UP = "UfinNextUp";
static const char* const HOME_FAVORITES = "UfinFavorites";
bool isHomeRow(const MediaItem& item);
MediaItem makeHomeRow(const char* type, int count);

// Watched tick, heart, resume bar and unwatched count for a list row.
void applyUserState(const MediaItem& item, ui::ListEntry& entry);
