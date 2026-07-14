#pragma once

#include <QString>

#include <stdint.h>
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
	// directory and initial label registry are created during construction.
	[[nodiscard]] static std::optional<Library> load(const QString& root, QString* error = nullptr);

	// Loads a complete candidate State before replacing the current one. Failure leaves every current object
	// and path untouched.
	[[nodiscard]] bool setRoot(const QString& root, QString* error = nullptr);

	[[nodiscard]] const QString& rootFolder() const;
	[[nodiscard]] QString photosRootFolder() const;
	[[nodiscard]] Catalog& catalog();
	[[nodiscard]] const Catalog& catalog() const;
	[[nodiscard]] MetadataStore& metadataStore();
	[[nodiscard]] const MetadataStore& metadataStore() const;
	[[nodiscard]] uint64_t generation() const { return m_generation; }

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
	Library(Library&&) noexcept;
	Library& operator=(Library&&) = delete;
	~Library();

private:
	explicit Library(QString root);

	std::unique_ptr<LibraryState> m_state;
	uint64_t m_generation = 0;
};
