#pragma once

#include <conc/concurrency.hpp>

#include <stdx/compiler.hpp>
#include <stdx/tuple.hpp>
#include <stdx/tuple_algorithms.hpp>

#include <limits>
#include <type_traits>

namespace interrupt {
enum class resource_status { OFF = 0, ON = 1 };

template <typename Irq>
constexpr static auto has_enable_field = requires { Irq::enable_field; };

template <typename RootT> struct dynamic_controller {
  private:
    /**
     * Store the interrupt enable values that are allowed given the current set
     * of resources that are available.
     *
     * @tparam RegType
     *      The croo::Register this mask corresponds to.
     */
    template <typename RegType>
    CONSTINIT static inline typename RegType::DataType allowed_enables =
        std::numeric_limits<typename RegType::DataType>::max();

    template <typename ResourceType> struct doesnt_require_resource {
        template <typename Irq>
        using fn = std::bool_constant<
            has_enable_field<Irq> and
            not stdx::contains_type<decltype(Irq::resources), ResourceType>>;
    };

    template <typename RegType> struct in_register {
        template <typename Field>
        using fn = std::is_same<RegType, typename Field::RegisterType>;
    };

    /**
     * For each ResourceType, keep track of what interrupts can still be enabled
     * when that resource goes down.
     *
     * Each bit in this mask corresponds to an interrupt enable field in
     * RegType. If the bit is '1', that means the corresponding interrupt can be
     * enabled when the resource is not available. If the bit is '0', that means
     * the corresponding interrupt must be disabled when the resource is not
     * available.
     *
     * @tparam ResourceType
     *      The resource we want to check.
     *
     * @tparam RegType
     *      The specific register mask we want to check.
     */
    template <typename ResourceType, typename RegType>
    constexpr static typename RegType::DataType irqs_allowed = []() {
        // get all interrupt enable fields that don't require the given resource
        auto const matching_irqs =
            stdx::filter<doesnt_require_resource<ResourceType>::template fn>(
                RootT::all_irqs);
        auto const interrupt_enables_tuple = stdx::transform(
            [](auto irq) { return irq.enable_field; }, matching_irqs);

        // filter fields that aren't in RegType
        auto const fields_in_reg =
            stdx::filter<in_register<RegType>::template fn>(
                interrupt_enables_tuple);

        // set the bits in the mask for interrupts that don't require the
        // resource
        using DataType = typename RegType::DataType;
        return fields_in_reg.fold_left(
            DataType{}, [](DataType value, auto field) -> DataType {
                return value | field.get_mask();
            });
    }();

    template <typename ResourceType>
    CONSTINIT static inline bool is_resource_on = true;

    template <typename RegTypeTuple>
    static inline void reprogram_interrupt_enables(RegTypeTuple regs) {
        stdx::for_each(
            [](auto reg) {
                using RegType = decltype(reg);

                // make sure we don't enable any interrupts that are not allowed
                // according to resource availability
                auto const final_enables =
                    allowed_enables<RegType> & dynamic_enables<RegType>;

                // update the hardware registers
                apply(write(reg.raw(final_enables)));
            },
            regs);
    }

    template <typename RegFieldTuple>
    constexpr static auto get_unique_regs(RegFieldTuple fields) {
        return fields.fold_left(stdx::make_tuple(), [](auto regs, auto field) {
            constexpr bool reg_has_been_seen_already =
                stdx::contains_type<decltype(regs),
                                    typename decltype(field)::RegisterType>;
            if constexpr (reg_has_been_seen_already) {
                return regs;
            } else {
                return stdx::tuple_cat(regs,
                                       stdx::make_tuple(field.get_register()));
            }
        });
    }

    template <typename ResourceTuple> struct not_in {
        template <typename Resource>
        using fn = std::bool_constant<
            not stdx::contains_type<ResourceTuple, Resource>>;
    };

    /**
     * tuple of every resource mentioned in the interrupt configuration
     */
    constexpr static auto all_resources = RootT::all_irqs.fold_left(
        stdx::make_tuple(), [](auto resources, auto irq) {
            // TODO: check that an IRQ doesn't list a resource more than once
            auto const additional_resources =
                stdx::filter<not_in<decltype(resources)>::template fn>(
                    irq.resources);
            return stdx::tuple_cat(resources, additional_resources);
        });

    /**
     * tuple of every interrupt register affected by a resource
     */
    constexpr static auto all_resource_affected_regs =
        get_unique_regs(RootT::all_irqs.fold_left(
            stdx::make_tuple(), [](auto registers, auto irq) {
                using irq_t = decltype(irq);
                constexpr bool depends_on_resources =
                    irq_t::resources.size() > 0u;
                if constexpr (has_enable_field<irq_t> && depends_on_resources) {
                    return stdx::tuple_cat(
                        registers,
                        stdx::make_tuple(irq.enable_field.get_register()));
                } else {
                    return registers;
                }
            }));

    /**
     * Reprogram interrupt enables based on updated resource availability.
     */
    static inline auto recalculate_allowed_enables() {
        // set allowed_enables mask for each resource affected register
        stdx::for_each(
            [](auto reg) {
                using RegType = decltype(reg);
                using DataType = typename RegType::DataType;
                allowed_enables<RegType> = std::numeric_limits<DataType>::max();
            },
            all_resource_affected_regs);

        // for each resource, if it is not on, mask out unavailable interrupts
        stdx::for_each(
            [=](auto resource) {
                using ResourceType = decltype(resource);
                if (not is_resource_on<ResourceType>) {
                    stdx::for_each(
                        [](auto reg) {
                            using RegType = decltype(reg);
                            allowed_enables<RegType> &=
                                irqs_allowed<ResourceType, RegType>;
                        },
                        all_resource_affected_regs);
                }
            },
            all_resources);

        return all_resource_affected_regs;
    }

    /**
     * Store the interrupt enable values that FW _wants_ at runtime,
     * irrespective of any resource conflicts that would require specific
     * interrupts to be disabled.
     *
     * @tparam RegType
     *      The croo::Register this value corresponds to.
     */
    template <typename RegType>
    CONSTINIT static inline typename RegType::DataType dynamic_enables{};

    template <typename... Callbacks> struct match_callback {
        template <typename Irq>
        using fn = std::bool_constant<
            has_enable_field<Irq> and
            (... or std::is_same_v<typename Irq::IrqCallbackType, Callbacks>)>;
    };

    template <bool en, typename... CallbacksToFind>
    static inline void enable_by_name() {
        // NOTE: critical section is not needed here because shared state is
        // only updated by the final call to enable_by_field

        // TODO: add support to enable/disable top-level IRQs by name.
        //       this will require another way to manage them vs. mmio
        //       registers. once that goes in, then enable_by_field should be
        //       removed or made private.
        auto const matching_irqs =
            stdx::filter<match_callback<CallbacksToFind...>::template fn>(
                RootT::all_irqs);

        auto const interrupt_enables_tuple = stdx::transform(
            [](auto irq) { return irq.enable_field; }, matching_irqs);

        interrupt_enables_tuple.apply([]<typename... Fields>(Fields...) {
            enable_by_field<en, Fields...>();
        });
    }

  public:
    template <typename ResourceType>
    static inline void update_resource(resource_status status) {
        conc::call_in_critical_section<dynamic_controller>([&] {
            is_resource_on<ResourceType> = (status == resource_status::ON);
            recalculate_allowed_enables();
            reprogram_interrupt_enables(all_resource_affected_regs);
        });
    }

    template <typename ResourceType> static inline void turn_on_resource() {
        update_resource<ResourceType>(resource_status::ON);
    }

    template <typename ResourceType> static inline void turn_off_resource() {
        update_resource<ResourceType>(resource_status::OFF);
    }

    template <bool en, typename... FieldsToSet>
    static inline void enable_by_field() {
        auto const interrupt_enables_tuple = stdx::tuple<FieldsToSet...>{};

        conc::call_in_critical_section<dynamic_controller>([&] {
            stdx::for_each(
                [](auto f) {
                    using RegType = decltype(f.get_register());
                    if constexpr (en) {
                        dynamic_enables<RegType> |= f.get_mask();
                    } else {
                        dynamic_enables<RegType> &= ~f.get_mask();
                    }
                },
                interrupt_enables_tuple);

            auto const unique_regs = get_unique_regs(interrupt_enables_tuple);
            reprogram_interrupt_enables(unique_regs);
        });
    }

    template <typename... CallbacksToFind> static inline void enable() {
        enable_by_name<true, CallbacksToFind...>();
    }

    template <typename... CallbacksToFind> static inline void disable() {
        enable_by_name<false, CallbacksToFind...>();
    }
};
} // namespace interrupt
