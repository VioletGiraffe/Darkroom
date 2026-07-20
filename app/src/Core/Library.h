#pragma once

#include <QString>

#include <stdint.h>
#include <functional>
#include <memory>

class Catalog;
class LibraryState;
class MetadataStore;

// The current Darkroom library. Its root, persistence store, and catalog form one private State that is
// replaced only after a new root has loaded successfully. The Library object itself is stable - immovable, and
// never rebuilt - so persistent collaborators can borrow it across root changes. GUI-thread only.
//
// A default-constructed Library is EMPTY: it holds no root, store or catalog, and only setRoot(),
// isLoaded(), flushPendingWrites() and pendingPersistenceError() may be called on it. That state exists so the
// owner can construct the Library as a plain member and load it afterwards; everything else asserts. Loading is
// setRoot()'s job whether it is the first root or a later one - there is deliberately no second way in.
class Library
{
public:
	Library();

	// Flushes the current state, then loads a complete candidate State before replacing it. Failure leaves every
	// current object and path untouched; unsaved current state can therefore never be discarded by switching.
	// The first successful call loads an empty Library: a missing root/files is a valid new library, so the root
	// directory and initial label registry are created here, and failing to save that registry fails the load.
	[[nodiscard]] bool setRoot(const QString& root, QString* error = nullptr);

	// Whether the folder has already been used as a library root. setRoot() does not care - a fresh folder is a
	// valid new library - so this exists for callers that must tell "create a new library here" apart from
	// "adopt the library that is already here".
	[[nodiscard]] static bool holdsLibrary(const QString& folder);

	// False until the first setRoot() succeeds. The accessors below assert on an empty Library rather than
	// inventing a value, so ask this only where "no library yet" is genuinely reachable - i.e. at startup.
	[[nodiscard]] bool isLoaded() const { return m_state != nullptr; }

	[[nodiscard]] const QString& rootFolder() const;
	[[nodiscard]] QString photosRootFolder() const;
	[[nodiscard]] Catalog& catalog();
	[[nodiscard]] const Catalog& catalog() const;
	[[nodiscard]] MetadataStore& metadataStore();
	[[nodiscard]] const MetadataStore& metadataStore() const;
	// Retries every dirty JSON store. A failed store remains dirty; error combines every path that still could
	// not be saved. The failure handler fires when a store first gains a pending error; the caller decides how
	// and when to present it. An empty Library has nothing to flush and no error to report.
	[[nodiscard]] bool flushPendingWrites(QString* error = nullptr);
	[[nodiscard]] QString pendingPersistenceError() const;
	void setPersistenceFailureHandler(std::function<void()> handler);
	[[nodiscard]] uint64_t generation() const { return m_generation; }

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
	Library(Library&&) = delete;
	Library& operator=(Library&&) = delete;
	~Library();

private:
	std::function<void()> m_persistenceFailureHandler;
	std::unique_ptr<LibraryState> m_state;
	uint64_t m_generation = 0;
};
