#pragma once

#include <QString>

#include <stdint.h>
#include <functional>
#include <memory>
#include <optional>

class Catalog;
class LibraryState;
class MetadataStore;

// The current Darkroom library. Its root, persistence store, and catalog form one private State that is
// replaced only after a new root has loaded successfully. The Library object itself remains stable so
// persistent collaborators can borrow it across root changes. GUI-thread only.
class Library
{
public:
	// Validates and loads the initial root. Returns null when its persistence files cannot be read or are not
	// JSON objects of the expected top-level shape. A missing root/files is a valid new library; the root
	// directory and initial label registry are created during construction; inability to save that initial
	// registry is also a load failure.
	[[nodiscard]] static std::optional<Library> load(const QString& root, QString* error = nullptr);

	// Flushes the current state, then loads a complete candidate State before replacing it. Failure leaves every
	// current object and path untouched; unsaved current state can therefore never be discarded by switching.
	[[nodiscard]] bool setRoot(const QString& root, QString* error = nullptr);

	[[nodiscard]] const QString& rootFolder() const;
	[[nodiscard]] QString photosRootFolder() const;
	[[nodiscard]] Catalog& catalog();
	[[nodiscard]] const Catalog& catalog() const;
	[[nodiscard]] MetadataStore& metadataStore();
	[[nodiscard]] const MetadataStore& metadataStore() const;
	// Retries every dirty JSON store. A failed store remains dirty; error combines every path that still could
	// not be saved. The failure handler fires when a store first gains a pending error; the caller decides how
	// and when to present it.
	[[nodiscard]] bool flushPendingWrites(QString* error = nullptr);
	[[nodiscard]] QString pendingPersistenceError() const;
	void setPersistenceFailureHandler(std::function<void()> handler);
	[[nodiscard]] uint64_t generation() const { return m_generation; }

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
	Library(Library&&) noexcept;
	Library& operator=(Library&&) = delete;
	~Library();

private:
	explicit Library(std::unique_ptr<LibraryState> state);

	std::function<void()> m_persistenceFailureHandler;
	std::unique_ptr<LibraryState> m_state;
	uint64_t m_generation = 0;
};
