/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Rocinante::Helpers {

namespace IntrusiveRedBlackTreeDetail {

template <typename Node, auto MemberPtr>
concept MemberObjectPointer =
	std::is_member_object_pointer_v<decltype(MemberPtr)> &&
	requires(const Node& node) {
		node.*MemberPtr;
};

template <typename Node, auto MemberPtr>
using MemberType = std::remove_cvref_t<decltype(std::declval<const Node&>().*MemberPtr)>;

} // namespace IntrusiveRedBlackTreeDetail

/**
 * @brief Intrusive red-black tree links embedded inside a node type.
 *
 * This link struct is intentionally small and explicit. The owning container
 * controls all mutations of these fields.
 */
template <typename Node>
struct IntrusiveRedBlackTreeLinks final {
	enum class Color : std::uint8_t {
		Red = 0,
		Black = 1,
	};

	Node* parent = nullptr;
	Node* left = nullptr;
	Node* right = nullptr;
	Color color = Color::Red;
	bool in_tree = false;
};

/**
 * @brief Intrusive red-black tree.
 *
 * This is a balanced binary search tree (BST) that stores no node allocations.
 * The caller owns node storage and embeds `IntrusiveRedBlackTreeLinks` in each
 * node.
 *
 * Template parameters:
 * - Node: the node type.
 * - LinksMember: pointer-to-member selecting the embedded links.
 * - KeyMember: pointer-to-member selecting the key field.
 *
 * Constraints / assumptions:
 * - Nodes must have stable addresses while inserted.
 * - A node may be inserted into at most one tree at a time.
 * - Duplicate keys are rejected.
 *
 * No runtime polymorphism: this is a compile-time generic type.
 */
template <
	typename Node,
	IntrusiveRedBlackTreeLinks<Node> Node::* LinksMember,
	auto KeyMember>
	requires IntrusiveRedBlackTreeDetail::MemberObjectPointer<Node, KeyMember> &&
		std::totally_ordered<IntrusiveRedBlackTreeDetail::MemberType<Node, KeyMember>>
class IntrusiveRedBlackTree {
public:
	using KeyType = IntrusiveRedBlackTreeDetail::MemberType<Node, KeyMember>;
	using LinksType = IntrusiveRedBlackTreeLinks<Node>;

	IntrusiveRedBlackTree() = default;
	~IntrusiveRedBlackTree() = default;
	IntrusiveRedBlackTree(const IntrusiveRedBlackTree&) = delete;
	IntrusiveRedBlackTree& operator=(const IntrusiveRedBlackTree&) = delete;
	IntrusiveRedBlackTree(IntrusiveRedBlackTree&&) = delete;
	IntrusiveRedBlackTree& operator=(IntrusiveRedBlackTree&&) = delete;

	std::size_t NodeCount() const { return m_node_count; }
	bool IsEmpty() const { return m_node_count == 0; }

	const Node* FindExact(KeyType key) const {
		const Node* node = m_root;
		while (node) {
			const KeyType node_key = KeyOf(node);
			if (key < node_key) {
				node = LinksOf(node).left;
				continue;
			}
			if (key > node_key) {
				node = LinksOf(node).right;
				continue;
			}
			return node;
		}
		return nullptr;
	}

	const Node* FindPredecessorOrEqual(KeyType key) const {
		const Node* best = nullptr;
		const Node* node = m_root;
		while (node) {
			const KeyType node_key = KeyOf(node);
			if (key < node_key) {
				node = LinksOf(node).left;
				continue;
			}
			best = node;
			node = LinksOf(node).right;
		}
		return best;
	}

	const Node* FindSuccessorOrEqual(KeyType key) const {
		const Node* best = nullptr;
		const Node* node = m_root;
		while (node) {
			const KeyType node_key = KeyOf(node);
			if (key > node_key) {
				node = LinksOf(node).right;
				continue;
			}
			best = node;
			node = LinksOf(node).left;
		}
		return best;
	}

	bool Insert(Node* node) {
		if (!node) return false;
		if (LinksOf(node).in_tree) return false;

		const KeyType key = KeyOf(node);

		Node* parent = nullptr;
		Node* current = m_root;
		while (current) {
			parent = current;
			const KeyType current_key = KeyOf(current);
			if (key < current_key) {
				current = LinksOf(current).left;
				continue;
			}
			if (key > current_key) {
				current = LinksOf(current).right;
				continue;
			}
			// Duplicate key.
			return false;
		}

		// Initialize links before linking into the tree.
		LinksOf(node).parent = parent;
		LinksOf(node).left = nullptr;
		LinksOf(node).right = nullptr;
		LinksOf(node).color = LinksType::Color::Red;
		LinksOf(node).in_tree = true;

		if (!parent) {
			m_root = node;
		} else if (key < KeyOf(parent)) {
			LinksOf(parent).left = node;
		} else {
			LinksOf(parent).right = node;
		}

		InsertFixup(node);
		m_node_count++;
		return true;
	}

	const Node* First() const { return Minimum(m_root); }

	const Node* Next(const Node* node) const {
		return Successor(node);
	}

	#if defined(ROCINANTE_TESTS)
	bool DebugValidateRbInvariants() const {
		if (!m_root) {
			return m_node_count == 0;
		}
		if (LinksOf(m_root).parent != nullptr) return false;
		if (LinksOf(m_root).color != LinksType::Color::Black) return false;

		ValidationState state{};
		ValidateNode(m_root, 0, &state);
		if (!state.ok) return false;
		if (state.node_count != m_node_count) return false;
		return true;
	}
	#endif

protected:
	const Node* Root() const { return m_root; }

private:
	static KeyType KeyOf(const Node* node) {
		return static_cast<KeyType>(node->*KeyMember);
	}

	static LinksType& LinksOf(Node* node) {
		return node->*LinksMember;
	}

	static const LinksType& LinksOf(const Node* node) {
		return node->*LinksMember;
	}

	static bool IsRed(const Node* node) {
		if (!node) return false;
		return LinksOf(node).color == LinksType::Color::Red;
	}

	static bool IsBlack(const Node* node) {
		if (!node) return true;
		return LinksOf(node).color == LinksType::Color::Black;
	}

	void RotateLeft(Node* pivot) {
		Node* right = LinksOf(pivot).right;
		if (!right) return;

		LinksOf(pivot).right = LinksOf(right).left;
		if (LinksOf(right).left) {
			LinksOf(LinksOf(right).left).parent = pivot;
		}

		LinksOf(right).parent = LinksOf(pivot).parent;
		if (!LinksOf(pivot).parent) {
			m_root = right;
		} else if (pivot == LinksOf(LinksOf(pivot).parent).left) {
			LinksOf(LinksOf(pivot).parent).left = right;
		} else {
			LinksOf(LinksOf(pivot).parent).right = right;
		}

		LinksOf(right).left = pivot;
		LinksOf(pivot).parent = right;
	}

	void RotateRight(Node* pivot) {
		Node* left = LinksOf(pivot).left;
		if (!left) return;

		LinksOf(pivot).left = LinksOf(left).right;
		if (LinksOf(left).right) {
			LinksOf(LinksOf(left).right).parent = pivot;
		}

		LinksOf(left).parent = LinksOf(pivot).parent;
		if (!LinksOf(pivot).parent) {
			m_root = left;
		} else if (pivot == LinksOf(LinksOf(pivot).parent).right) {
			LinksOf(LinksOf(pivot).parent).right = left;
		} else {
			LinksOf(LinksOf(pivot).parent).left = left;
		}

		LinksOf(left).right = pivot;
		LinksOf(pivot).parent = left;
	}

	void InsertFixup(Node* node) {
		while (node != m_root && IsRed(LinksOf(node).parent)) {
			Node* parent = LinksOf(node).parent;
			Node* grandparent = parent ? LinksOf(parent).parent : nullptr;
			if (!grandparent) break;

			if (parent == LinksOf(grandparent).left) {
				Node* uncle = LinksOf(grandparent).right;
				if (IsRed(uncle)) {
					LinksOf(parent).color = LinksType::Color::Black;
					LinksOf(uncle).color = LinksType::Color::Black;
					LinksOf(grandparent).color = LinksType::Color::Red;
					node = grandparent;
					continue;
				}

				if (node == LinksOf(parent).right) {
					node = parent;
					RotateLeft(node);
					parent = LinksOf(node).parent;
					grandparent = parent ? LinksOf(parent).parent : nullptr;
					if (!grandparent) break;
				}

				LinksOf(parent).color = LinksType::Color::Black;
				LinksOf(grandparent).color = LinksType::Color::Red;
				RotateRight(grandparent);
			} else {
				Node* uncle = LinksOf(grandparent).left;
				if (IsRed(uncle)) {
					LinksOf(parent).color = LinksType::Color::Black;
					LinksOf(uncle).color = LinksType::Color::Black;
					LinksOf(grandparent).color = LinksType::Color::Red;
					node = grandparent;
					continue;
				}

				if (node == LinksOf(parent).left) {
					node = parent;
					RotateRight(node);
					parent = LinksOf(node).parent;
					grandparent = parent ? LinksOf(parent).parent : nullptr;
					if (!grandparent) break;
				}

				LinksOf(parent).color = LinksType::Color::Black;
				LinksOf(grandparent).color = LinksType::Color::Red;
				RotateLeft(grandparent);
			}
		}

		if (m_root) {
			LinksOf(m_root).color = LinksType::Color::Black;
		}
	}

	static const Node* Minimum(const Node* node) {
		if (!node) return nullptr;
		const Node* current = node;
		while (LinksOf(current).left) {
			current = LinksOf(current).left;
		}
		return current;
	}

	static const Node* Successor(const Node* node) {
		if (!node) return nullptr;
		if (LinksOf(node).right) {
			return Minimum(LinksOf(node).right);
		}

		const Node* current = node;
		const Node* parent = LinksOf(current).parent;
		while (parent && current == LinksOf(parent).right) {
			current = parent;
			parent = LinksOf(parent).parent;
		}
		return parent;
	}

	#if defined(ROCINANTE_TESTS)
	struct ValidationState final {
		bool ok = true;
		std::size_t node_count = 0;
		const Node* previous = nullptr;
		bool expected_black_height_is_set = false;
		std::size_t expected_black_height = 0;
	};

	static void ValidateNode(const Node* node, std::size_t black_count, ValidationState* state) {
		if (!state || !state->ok) return;
		if (!node) {
			if (!state->expected_black_height_is_set) {
				state->expected_black_height = black_count;
				state->expected_black_height_is_set = true;
				return;
			}
			if (black_count != state->expected_black_height) {
				state->ok = false;
			}
			return;
		}

		if (LinksOf(node).color == LinksType::Color::Black) {
			black_count++;
		} else {
			if (!IsBlack(LinksOf(node).left)) state->ok = false;
			if (!IsBlack(LinksOf(node).right)) state->ok = false;
		}

		if (LinksOf(node).left && LinksOf(LinksOf(node).left).parent != node) state->ok = false;
		if (LinksOf(node).right && LinksOf(LinksOf(node).right).parent != node) state->ok = false;
		if (!LinksOf(node).in_tree) state->ok = false;

		ValidateNode(LinksOf(node).left, black_count, state);

		state->node_count++;
		if (state->previous) {
			const KeyType prev_key = KeyOf(state->previous);
			const KeyType this_key = KeyOf(node);
			if (this_key <= prev_key) state->ok = false;
		}
		state->previous = node;

		ValidateNode(LinksOf(node).right, black_count, state);
	}
	#endif

	Node* m_root = nullptr;
	std::size_t m_node_count = 0;
};

} // namespace Rocinante::Helpers
