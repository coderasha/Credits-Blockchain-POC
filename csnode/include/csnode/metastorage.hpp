#ifndef METASTORAGE_HPP
#define METASTORAGE_HPP

#include <optional>
#include <lib/system/common.hpp>
#include <boost/circular_buffer.hpp>

namespace cs
{
    ///
    /// @brief Class for storage meta information with
    /// common interface and dependency of round number.
    ///
    template<typename T>
    class MetaStorage
    {
    public:
        ///
        /// @brief Represents meta element with storage
        /// of round and generated (instantiated) meta class.
        ///
        struct MetaElement
        {
            RoundNumber round;
            T meta;

            bool operator ==(const MetaElement& element)
            {
                return round == element.round;
            }

            bool operator !=(const MetaElement& element)
            {
                return !((*this) == element);
            }
        };

        /// element type
        using Element = MetaStorage<T>::MetaElement;

        // storage interface

        ///
        /// @brief Resizes circular storage buffer.
        ///
        void resize(std::size_t size)
        {
            m_buffer.resize(size);
        }

        ///
        /// @brief Appends meta element to buffer.
        /// @param element Created from outside rvalue.
        /// @return Returns true if append is success, otherwise returns false.
        ///
        bool append(MetaElement&& value)
        {
            const auto iterator = std::find(m_buffer.begin(), m_buffer.end(), value);
            const auto result = (iterator == m_buffer.end());

            if (result) {
                m_buffer.push_back(std:move(value));
            }

            return result;
        }

        ///
        /// @brief Appends meta element to buffer.
        /// @param element Created from outside no need lvalue, cuz it would be moved.
        /// @return Returns true if append is success, otherwise returns false.
        ///
        bool append(MetaElement& value)
        {
            return append(std::move(value));
        }

        ///
        /// @brief Returns contains meta storage this round or not.
        ///
        bool contains(RoundNumber round)
        {
            const auto iterator = std::find_if(m_buffer.begin(), m_buffer.end(), [=](const MetaElement& value) {
                return value.round == round;
            });

            return iterator != m_buffer.end();
        }

        ///
        /// @brief Returns meta element if this round exists at storage, otherwise returns nothing.
        /// @param round Round number to get element from storage.
        /// @return Returns optional parameter.
        ///
        std::optional<MetaElement> get(RoundNumber round)
        {
            const auto iterator = std::find_if(m_buffer.begin(), m_buffer.end(), [=](const MetaElement& value) {
                return value.round == round;
            });

            if (iterator != m_buffer.end()) {
                return *iterator;
            }

            return std::nullopt;
        }

        ///
        /// @brief Returns and removes meta element if it's exists at round parameter.
        /// @param round Round number to get element from storage.
        /// @return Returns meta element of storage if found, otherwise default constructed meta element.
        ///.
        MetaElement pop(RoundNumber round)
        {
            MetaElement result;

            const auto iterator = std::find_if(m_buffer.begin(), m_buffer.end(), [=](const MetaElement& value) {
                return value.round == round;
            });

            if (iterator != m_buffer.end()) {
                result = std::move(*iterator);
                m_buffer.erase(iterator);
            }

            return result;
        }

    private:
        boost::circular_buffer<MetaElement> m_buffer;
    };
}

#endif  // METASTORAGE_HPP
