#include <algorithm>
#include <array>
#include <stdexcept>
#include <initializer_list>

namespace fsp
{
  using cstr_t = std::string_view;
  // Dummy class for context
  struct xpath_el
  {
  public:
    cstr_t ns;
    cstr_t tag;
  };
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  class static_xpath_vec
  {
  private:
    static constexpr auto          vec_size = 100;
    std::array<xpath_el, vec_size> arr_{};
    size_t                         size_ = 0;
  public:
    // Types required to match std::vector API
    using value_type      = xpath_el;
    using size_type       = size_t;
    using reference       = xpath_el&;
    using const_reference = const xpath_el&;
    using iterator        = xpath_el*;
    using const_iterator  = const xpath_el*;

    // --- Constructors & Assignment ---
    constexpr static_xpath_vec() noexcept                 = default;
    constexpr ~static_xpath_vec()                         = default;
    constexpr static_xpath_vec(const static_xpath_vec& o) = default;
    constexpr static_xpath_vec(static_xpath_vec&& o)      = default;

    constexpr static_xpath_vec(std::initializer_list<xpath_el> init)
    {
      if (init.size() > vec_size) throw std::out_of_range("Initializer list exceeds capacity of 100");
      std::ranges::copy(init, arr_.begin());
      size_ = init.size();
    }

    constexpr static_xpath_vec& operator=(const static_xpath_vec& other)
    {
      if (this != &other)
      {
        arr_  = other.arr_;
        size_ = other.size_;
      }
      return *this;
    }

    constexpr static_xpath_vec& operator=(static_xpath_vec&& other) noexcept
    {
      if (this != &other)
      {
        arr_        = other.arr_;
        size_       = other.size_;
        other.size_ = 0;
      }
      return *this;
    }

    constexpr static_xpath_vec& operator=(std::initializer_list<xpath_el> ilist)
    {
      if (ilist.size() > vec_size) throw std::out_of_range("Initializer list exceeds capacity of 100");
      std::ranges::copy(ilist, arr_.begin());
      size_ = ilist.size();
      return *this;
    }

    // --- Element Access ---
    constexpr reference at(size_type pos)
    {
      if (pos >= size_) throw std::out_of_range("xx::at() - index out of range");
      return arr_.at(pos);
    }

    [[nodiscard]] constexpr const_reference at(size_type pos) const
    {
      if (pos >= size_) throw std::out_of_range("xx::at() - index out of range (const)");
      return arr_.at(pos);
    }

    constexpr reference       operator[](size_type pos) { return arr_[pos]; }
    constexpr const_reference operator[](size_type pos) const { return arr_[pos]; }

    constexpr reference                     front() { return arr_[0]; }
    [[nodiscard]] constexpr const_reference front() const { return arr_[0]; }

    constexpr reference                     back() { return arr_[size_ - 1]; }
    [[nodiscard]] constexpr const_reference back() const { return arr_[size_ - 1]; }

    // --- Iterators ---
    constexpr iterator                     begin() noexcept { return arr_.data(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return arr_.data(); }
    constexpr iterator                     end() noexcept { return &arr_[size_]; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return &arr_[size_]; }

    // --- Capacity ---
    [[nodiscard]] constexpr bool      empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr size_type max_size() const noexcept { return vec_size; }
    [[nodiscard]] constexpr size_type capacity() const noexcept { return vec_size; }

    // --- Modifiers ---
    constexpr void clear() noexcept { size_ = 0; }

    constexpr void push_back(const xpath_el& value)
    {
      if (size_ >= vec_size) throw std::out_of_range("xx::push_back() - capacity exceeded");
      arr_[size_] = value;
      size_++;
    }

    // constexpr void push_back(xpath_el&& value)
    // {
    //   if (size_ >= vec_size) throw std::out_of_range("xx::push_back() - capacity exceeded");
    //   arr_[size_] = value;
    //   size_++;
    // }

    template <class... Args>
    constexpr reference emplace_back(Args&&... args)
    {
      if (size_ >= vec_size) throw std::out_of_range("xx::emplace_back() - capacity exceeded");

      // Correct implementation: construct directly in-place using placement new
      // and safely increment size_ AFTER construction
      reference ref = arr_[size_] = xpath_el(std::forward<Args>(args)...);
      size_++;
      return ref;
    }

    constexpr void pop_back()
    {
      if (size_ > 0) { size_--; }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  };
} // namespace fsp