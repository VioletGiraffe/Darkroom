#pragma once

// MIME type for dragging a label (carried as its Catalog id, UTF-8) from the LabelSidebar onto a video
// card to assign it. Produced by LabelSidebar; recognized by MediaItemWidget as a drop it accepts
// (CopyAction = add the label, never remove). Shared here so the string isn't duplicated across the two.
inline constexpr char LabelMimeType[] = "application/x-darkroom-label";
