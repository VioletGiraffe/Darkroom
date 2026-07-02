#pragma once

#include "Core/Catalog.h"
#include "Core/VideoId.h"

#include <QDialog>

#include <functional>

// ============================================================================
// IntegrityCheckDialog - scans the catalog against disk for the three kinds of drift Catalog::scanIntegrity
// can find (relinkable placeholders, untracked frame folders, ghost entries) and lets the user resolve each
// one individually: relink/register/re-import/remove, or browse to manually point at a source video where
// the catalog couldn't resolve one on its own. All scan/UI logic lives here, behind the static
// scanAndShowUi() entry point - MainWindow only supplies the callbacks that actually touch the Catalog/disk.
// ============================================================================

class IntegrityCheckDialog final : public QDialog
{
public:
	struct Callbacks
	{
		// Attempts to relink a placeholder to the (now-confirmed-to-exist) source path; returns whether it
		// succeeded (Catalog::relinkPlaceholder refuses if that identity is already tracked elsewhere).
		std::function<bool(const VideoId& placeholderId, const QString& confirmedSourcePath)> relinkRequested;
		// Attempts to register an untracked folder at the given source path; returns whether it succeeded
		// (Catalog::addVideo refuses on an id clash with a different folder).
		std::function<bool(const QString& folderPath, const QString& sourcePath)> registerRequested;
		// Re-extracts frames for a ghost whose source is still present, landing back in its existing folder;
		// returns whether frames actually exist there afterwards.
		std::function<bool(const VideoId& ghostId)> reimportRequested;
		// Drops a ghost entry from the catalog outright (its folder is already gone); always succeeds.
		std::function<bool(const VideoId& ghostId)> removeGhostRequested;
	};

	// Scans the catalog for drift against disk and, if anything was found, shows this dialog so the user can
	// resolve each finding. Just an information box if the catalog is clean.
	static void scanAndShowUi(Callbacks callbacks, QWidget* parent);

private:
	IntegrityCheckDialog(const Catalog::IntegrityReport& report, Callbacks callbacks, QWidget* parent);
	~IntegrityCheckDialog() override;

	Callbacks m_callbacks;
};
