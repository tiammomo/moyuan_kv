namespace cpputil {

template <typename T>
class Iterator {
    virtual Iterator& operator ++() = 0;
    virtual Iterator& operator --() = 0;
    virtual T& operator *() = 0;
};

} // cpputil