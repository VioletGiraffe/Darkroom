#pragma once

#include <QComboBox>

// Shared preview-frame-count selector. Persistence and the resulting view/extraction work remain with its owner.
class PreviewFrameCountCombo final : public QComboBox
{
public:
	explicit PreviewFrameCountCombo(int initialFrameCount, QWidget* parent = nullptr);

	[[nodiscard]] int frameCount() const;
};
