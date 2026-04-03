#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "mokv/cache/iterator.h"
namespace cpputil {

namespace list {

struct Void {};

template <typename T>
struct Node {
    struct PassBy { // use & for > 16 bit size object
        using type = typename std::conditional<(sizeof(T) > (sizeof(void*) << 1)) || !std::is_trivially_copyable_v<T>, T&, T>::type;
    };
    Node() = default;
    template <typename U = T, typename std::enable_if<!std::is_same_v<U, typename PassBy::type>, int>::type = 0>
    Node(T&& value) {
        this->value = std::move(value);
    }
    Node(const T& value) {
        this->value = value;
    }
    // Node(const typename PassBy::type value) {
    //     this->value = value;
    // }
    T value;
    std::atomic_size_t promotions{0};
    Node *nxt = nullptr;
    Node *pre = nullptr;
};

template <typename T, typename List>
class ListView;

template <typename T>
class List {
public:
    struct NodeBuilder {
        Node<T>* Build(T&& value) {
            return new Node<T>(std::forward<T>(value));
        }
    };

    class Iterator : public ::cpputil::Iterator<T> {
      public:
        Iterator() = default;
        Iterator& operator = (const Iterator&) = default;
        Iterator& operator = (Iterator&&) = default;
        Iterator(Node<T>* it): it_(it) {}
        Iterator& operator ++() override {
            it_ = it_->nxt;
            return *this;
        }
        Iterator& operator --() override {
            it_ = it_->pre;
            return *this;
        }
        T& operator *() override {
            return it_->value;
        }
        bool operator != (const Iterator& rhs) {
            return it_ != rhs.it_;
        }
      private:
        Node<T>* it_;
    };

    List(const List&) = delete;
    List& operator = (const List&) = delete;
    List(List&& list) {
        dummy_ = list.dummy_;
        size_ = list.size_;
        list.dummy_ = nullptr;
        list.size_ = 0;
    }
    List& operator = (List&& list) {
        dummy_ = list.dummy_;
        size_ = list.size_;
        list.dummy_ = nullptr;
        list.size_ = 0;
    }
    List() {
        dummy_ = new Node<T>();
        dummy_->nxt = dummy_;
        dummy_->pre = dummy_;
    }
    ~List() {
        std::vector<Node<T>*> nodes;
        nodes.reserve(size_ + 1);
        nodes.emplace_back(dummy_);
        for (auto i = dummy_->nxt; i != dummy_; i = i->nxt) {
            nodes.emplace_back(i);
        }
        for (auto x : nodes) {
            delete x;
        }
    }
    static NodeBuilder NodeBuilder() {
        return NodeBuilder();
    }
    inline Node<T>* PushFront(T&& value) {
        Node<T>* new_node = new Node<T>(std::move(value));
        new_node->nxt = dummy_->nxt;
        new_node->pre = dummy_;
        dummy_->nxt->pre = new_node;
        dummy_->nxt = new_node;
        ++size_;
        return new_node;
    }
    inline Node<T>* PushFront(const T& value) {
        Node<T>* new_node = new Node<T>(value);
        new_node->nxt = dummy_->nxt;
        new_node->pre = dummy_;
        dummy_->nxt->pre = new_node;
        dummy_->nxt = new_node;
        ++size_;
        return new_node;
    }
    inline Node<T>* PushBack(const T& value) {
        Node<T>* new_node = new Node<T>(value);
        new_node->pre = dummy_->pre;
        new_node->nxt = dummy_;
        dummy_->pre->nxt = new_node;
        dummy_->pre = new_node;
        ++size_;
        return new_node;
    }
    inline Node<T>* PushBack(T&& value) {
        Node<T>* new_node = new Node<T>(std::forward<T>(value));
        new_node->pre = dummy_->pre;
        new_node->nxt = dummy_;
        dummy_->pre->nxt = new_node;
        dummy_->pre = new_node;
        ++size_;
        return new_node;
    }
    inline std::unique_ptr<Node<T>> PopFront() {
        if (!size_) {
            return nullptr;
        }
        --size_;
        return std::unique_ptr<Node<T>>(Extract(dummy_->nxt));
    }
    inline std::unique_ptr<Node<T>> PopBack() {
        if (!size_) {
            return nullptr;
        }
        --size_;
        return std::unique_ptr<Node<T>>(Extract(dummy_->pre));
    }
    /*
    * @attention: size_ not change
    */
    inline void InsertFront(Node<T>* node) {
        node->nxt = dummy_->nxt;
        node->pre = dummy_;
        dummy_->nxt->pre = node;
        dummy_->nxt = node;
    }
    inline void Insert(Node<T>* node, T&& value) {
        Node<T>* new_node = new Node<T>(std::move(value));
        new_node->nxt = node->nxt;
        new_node->pre = node;
        node->nxt = new_node;
    }
    inline void Insert(Node<T>* node, const T& value) {
        Node<T>* new_node = new Node<T>(std::forward<T>(value));
        new_node->nxt = node->nxt;
        new_node->pre = node;
        node->nxt = new_node;
    }
    /*
    * @attention: size_ not change
    */
    inline static Node<T>* Extract(Node<T>* node) {
        node->pre->nxt = node->nxt;
        node->nxt->pre = node->pre;
        return node;
    }
    inline size_t size() const {
        return size_;
    }
    inline Iterator begin() const {
        return Iterator(dummy_->nxt);   
    }
    inline Iterator end() const {
        return Iterator(dummy_);
    }
private:
    friend ListView<T, List<T>>;
    Node<T>* dummy_;
    size_t size_ = 0;
};


template <typename T, typename ListType = List<T> >
class ListView {
  public:
    ListView(const ListType& list) {
        begin_ = list.begin();
        end_ = list.end();
    }
    typename ListType::Iterator& begin() {
        return begin_;
    }
    typename ListType::Iterator& end() {
        return end_;
    }
  private:
    typename ListType::Iterator begin_;
    typename ListType::Iterator end_;
};

} // list

} // cpputil