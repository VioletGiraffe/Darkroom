#pragma once

#include <QObject>
#include <QString>

#include <stdint.h>
#include <functional>
#include <memory>

class Catalog;
class LibraryState;
class MetadataStore;

// Stable, immovable GUI-thread owner of one root-bound Catalog and MetadataStore. setRoot() atomically replaces
// that private state, so persistent collaborators may borrow Library across root changes. A default Library is
// empty; only loading/status/persistence operations are valid until setRoot() succeeds.
class Library : public QObject
{
	Q_OBJECT
public:
	Library();

	// Flushes first, then loads a complete candidate before replacement. Failure preserves the current state.
	// Missing roots/files create a new library; failure to persist its initial registry fails the load.
	[[nodiscard]] bool setRoot(const QString& root, QString* error = nullptr);

	// Distinguishes "create here" from adopting an existing library; setRoot() accepts either.
	[[nodiscard]] static bool holdsLibrary(const QString& folder);

	// Accessors below assert until the first successful setRoot().
	[[nodiscard]] bool isLoaded() const { return _state != nullptr; }

	[[nodiscard]] const QString& rootFolder() const;
	[[nodiscard]] QString photosRootFolder() const;
	[[nodiscard]] Catalog& catalog();
	[[nodiscard]] const Catalog& catalog() const;
	[[nodiscard]] MetadataStore& metadataStore();
	[[nodiscard]] const MetadataStore& metadataStore() const;
	// Retries every dirty JSON store. Failures remain dirty and error combines their paths. An empty Library succeeds.
	[[nodiscard]] bool flushPendingWrites(QString* error = nullptr);
	[[nodiscard]] QString pendingPersistenceError() const;
	void setPersistenceFailureHandler(std::function<void()> handler);
	[[nodiscard]] uint64_t generation() const { return _generation; }

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
	Library(Library&&) = delete;
	Library& operator=(Library&&) = delete;
	~Library() override;

signals:
	// Emitted after a successful root replacement or a browser-visible change to the active Catalog.
	void catalogChanged();

private:
	std::function<void()> _persistenceFailureHandler;
	std::unique_ptr<LibraryState> _state;
	uint64_t _generation = 0;
};
