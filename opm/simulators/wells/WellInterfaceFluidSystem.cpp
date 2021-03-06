/*
  Copyright 2017 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2017 Statoil ASA.
  Copyright 2018 IRIS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>
#include <opm/simulators/wells/WellInterfaceFluidSystem.hpp>

#include <opm/material/fluidsystems/BlackOilFluidSystem.hpp>

#include <opm/parser/eclipse/EclipseState/Schedule/Well/WellTestState.hpp>

#include <opm/simulators/utils/DeferredLogger.hpp>
#include <opm/simulators/wells/RateConverter.hpp>
#include <opm/simulators/wells/ParallelWellInfo.hpp>
#include <opm/simulators/wells/WellGroupHelpers.hpp>
#include <opm/simulators/wells/WellState.hpp>

#include <ebos/eclalternativeblackoilindices.hh>

#include <cassert>
#include <cmath>

namespace Opm
{

template<class FluidSystem>
WellInterfaceFluidSystem<FluidSystem>::
WellInterfaceFluidSystem(const Well& well,
                         const ParallelWellInfo& parallel_well_info,
                         const int time_step,
                         const RateConverterType& rate_converter,
                         const int pvtRegionIdx,
                         const int num_components,
                         const int num_phases,
                         const int index_of_well,
                         const int first_perf_index,
                         const std::vector<PerforationData>& perf_data)
    : WellInterfaceGeneric(well, parallel_well_info, time_step,
                           pvtRegionIdx, num_components, num_phases,
                           index_of_well, first_perf_index, perf_data)
    , rateConverter_(rate_converter)
{
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
calculateReservoirRates(WellState& well_state) const
{
    const int fipreg = 0; // not considering the region for now
    const int np = number_of_phases_;

    std::vector<double> surface_rates(np, 0.0);
    for (int p = 0; p < np; ++p) {
        surface_rates[p] = well_state.wellRates(index_of_well_)[p];
    }

    std::vector<double> voidage_rates(np, 0.0);
    rateConverter_.calcReservoirVoidageRates(fipreg, pvtRegionIdx_, surface_rates, voidage_rates);

    for (int p = 0; p < np; ++p) {
        well_state.wellReservoirRates(index_of_well_)[p] = voidage_rates[p];
    }
}

template <typename FluidSystem>
bool
WellInterfaceFluidSystem<FluidSystem>::
checkIndividualConstraints(WellState& well_state,
                           const SummaryState& summaryState) const
{
    const auto& well = well_ecl_;
    const PhaseUsage& pu = phaseUsage();
    const int well_index = index_of_well_;

    if (well.isInjector()) {
        const auto controls = well.injectionControls(summaryState);
        auto currentControl = well_state.currentInjectionControl(well_index);

        if (controls.hasControl(Well::InjectorCMode::BHP) && currentControl != Well::InjectorCMode::BHP)
        {
            const auto& bhp = controls.bhp_limit;
            double current_bhp = well_state.bhp(well_index);
            if (bhp < current_bhp) {
                well_state.currentInjectionControl(well_index, Well::InjectorCMode::BHP);
                return true;
            }
        }

        if (controls.hasControl(Well::InjectorCMode::RATE) && currentControl != Well::InjectorCMode::RATE)
        {
            InjectorType injectorType = controls.injector_type;
            double current_rate = 0.0;

            switch (injectorType) {
            case InjectorType::WATER:
            {
                current_rate = well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Aqua] ];
                break;
            }
            case InjectorType::OIL:
            {
                current_rate = well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Liquid] ];
                break;
            }
            case InjectorType::GAS:
            {
                current_rate = well_state.wellRates(well_index)[  pu.phase_pos[BlackoilPhases::Vapour] ];
                break;
            }
            default:
                throw("Expected WATER, OIL or GAS as type for injectors " + well.name());
            }

            if (controls.surface_rate < current_rate) {
                well_state.currentInjectionControl(well_index, Well::InjectorCMode::RATE);
                return true;
            }

        }

        if (controls.hasControl(Well::InjectorCMode::RESV) && currentControl != Well::InjectorCMode::RESV)
        {
            double current_rate = 0.0;
            if( pu.phase_used[BlackoilPhases::Aqua] )
                current_rate += well_state.wellReservoirRates(well_index)[ pu.phase_pos[BlackoilPhases::Aqua] ];

            if( pu.phase_used[BlackoilPhases::Liquid] )
                current_rate += well_state.wellReservoirRates(well_index)[ pu.phase_pos[BlackoilPhases::Liquid] ];

            if( pu.phase_used[BlackoilPhases::Vapour] )
                current_rate += well_state.wellReservoirRates(well_index)[ pu.phase_pos[BlackoilPhases::Vapour] ];

            if (controls.reservoir_rate < current_rate) {
                currentControl = Well::InjectorCMode::RESV;
                return true;
            }
        }

        if (controls.hasControl(Well::InjectorCMode::THP) && currentControl != Well::InjectorCMode::THP)
        {
            const auto& thp = getTHPConstraint(summaryState);
            double current_thp = well_state.thp(well_index);
            if (thp < current_thp) {
                currentControl = Well::InjectorCMode::THP;
                return true;
            }
        }

    }

    if (well.isProducer( )) {
        const auto controls = well.productionControls(summaryState);
        auto currentControl = well_state.currentProductionControl(well_index);

        if (controls.hasControl(Well::ProducerCMode::BHP) && currentControl != Well::ProducerCMode::BHP )
        {
            const double bhp = controls.bhp_limit;
            double current_bhp = well_state.bhp(well_index);
            if (bhp > current_bhp) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::BHP);
                return true;
            }
        }

        if (controls.hasControl(Well::ProducerCMode::ORAT) && currentControl != Well::ProducerCMode::ORAT) {
            double current_rate = -well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Liquid] ];
            if (controls.oil_rate < current_rate  ) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::ORAT);
                return true;
            }
        }

        if (controls.hasControl(Well::ProducerCMode::WRAT) && currentControl != Well::ProducerCMode::WRAT ) {
            double current_rate = -well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Aqua] ];
            if (controls.water_rate < current_rate  ) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::WRAT);
                return true;
            }
        }

        if (controls.hasControl(Well::ProducerCMode::GRAT) && currentControl != Well::ProducerCMode::GRAT ) {
            double current_rate = -well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Vapour] ];
            if (controls.gas_rate < current_rate  ) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::GRAT);
                return true;
            }
        }

        if (controls.hasControl(Well::ProducerCMode::LRAT) && currentControl != Well::ProducerCMode::LRAT) {
            double current_rate = -well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Liquid] ];
            current_rate -= well_state.wellRates(well_index)[ pu.phase_pos[BlackoilPhases::Aqua] ];
            if (controls.liquid_rate < current_rate  ) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::LRAT);
                return true;
            }
        }

        if (controls.hasControl(Well::ProducerCMode::RESV) && currentControl != Well::ProducerCMode::RESV ) {
            double current_rate = 0.0;
            if( pu.phase_used[BlackoilPhases::Aqua] )
                current_rate -= well_state.wellReservoirRates(well_index)[ pu.phase_pos[BlackoilPhases::Aqua] ];

            if( pu.phase_used[BlackoilPhases::Liquid] )
                current_rate -= well_state.wellReservoirRates(well_index)[ pu.phase_pos[BlackoilPhases::Liquid] ];

            if( pu.phase_used[BlackoilPhases::Vapour] )
                current_rate -= well_state.wellReservoirRates(well_index)[ pu.phase_pos[BlackoilPhases::Vapour] ];

            if (controls.prediction_mode && controls.resv_rate < current_rate) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::RESV);
                return true;
            }

            if (!controls.prediction_mode) {
                const int fipreg = 0; // not considering the region for now
                const int np = number_of_phases_;

                std::vector<double> surface_rates(np, 0.0);
                if( pu.phase_used[BlackoilPhases::Aqua] )
                    surface_rates[pu.phase_pos[BlackoilPhases::Aqua]] = controls.water_rate;
                if( pu.phase_used[BlackoilPhases::Liquid] )
                    surface_rates[pu.phase_pos[BlackoilPhases::Liquid]] = controls.oil_rate;
                if( pu.phase_used[BlackoilPhases::Vapour] )
                    surface_rates[pu.phase_pos[BlackoilPhases::Vapour]] = controls.gas_rate;

                std::vector<double> voidage_rates(np, 0.0);
                rateConverter_.calcReservoirVoidageRates(fipreg, pvtRegionIdx_, surface_rates, voidage_rates);

                double resv_rate = 0.0;
                for (int p = 0; p < np; ++p) {
                    resv_rate += voidage_rates[p];
                }

                if (resv_rate < current_rate) {
                    well_state.currentProductionControl(well_index, Well::ProducerCMode::RESV);
                    return true;
                }
            }
        }

        if (controls.hasControl(Well::ProducerCMode::THP) && currentControl != Well::ProducerCMode::THP)
        {
            const auto& thp = getTHPConstraint(summaryState);
            double current_thp =  well_state.thp(well_index);
            if (thp > current_thp) {
                well_state.currentProductionControl(well_index, Well::ProducerCMode::THP);
                return true;
            }
        }

    }

    return false;
}

template <typename FluidSystem>
std::pair<bool, double>
WellInterfaceFluidSystem<FluidSystem>::
checkGroupConstraintsInj(const Group& group,
                         const WellState& well_state,
                         const GroupState& group_state,
                         const double efficiencyFactor,
                         const Schedule& schedule,
                         const SummaryState& summaryState,
                         DeferredLogger& deferred_logger) const
{
    // Translate injector type from control to Phase.
    const auto& well_controls = this->well_ecl_.injectionControls(summaryState);
    auto injectorType = well_controls.injector_type;
    Phase injectionPhase;
    switch (injectorType) {
    case InjectorType::WATER:
    {
        injectionPhase = Phase::WATER;
        break;
    }
    case InjectorType::OIL:
    {
        injectionPhase = Phase::OIL;
        break;
    }
    case InjectorType::GAS:
    {
        injectionPhase = Phase::GAS;
        break;
    }
    default:
        throw("Expected WATER, OIL or GAS as type for injector " + name());
    }

    // Make conversion factors for RESV <-> surface rates.
    std::vector<double> resv_coeff(phaseUsage().num_phases, 1.0);
    rateConverter_.calcCoeff(0, pvtRegionIdx_, resv_coeff); // FIPNUM region 0 here, should use FIPNUM from WELSPECS.

    // Call check for the well's injection phase.
    return WellGroupHelpers::checkGroupConstraintsInj(name(),
                                                      well_ecl_.groupName(),
                                                      group,
                                                      well_state,
                                                      group_state,
                                                      current_step_,
                                                      guide_rate_,
                                                      well_state.wellRates(index_of_well_).data(),
                                                      injectionPhase,
                                                      phaseUsage(),
                                                      efficiencyFactor,
                                                      schedule,
                                                      summaryState,
                                                      resv_coeff,
                                                      deferred_logger);
}

template <typename FluidSystem>
std::pair<bool, double>
WellInterfaceFluidSystem<FluidSystem>::
checkGroupConstraintsProd(const Group& group,
                          const WellState& well_state,
                          const GroupState& group_state,
                          const double efficiencyFactor,
                          const Schedule& schedule,
                          const SummaryState& summaryState,
                          DeferredLogger& deferred_logger) const
{
    // Make conversion factors for RESV <-> surface rates.
    std::vector<double> resv_coeff(this->phaseUsage().num_phases, 1.0);
    rateConverter_.calcCoeff(0, pvtRegionIdx_, resv_coeff); // FIPNUM region 0 here, should use FIPNUM from WELSPECS.

    return WellGroupHelpers::checkGroupConstraintsProd(name(),
                                                       well_ecl_.groupName(),
                                                       group,
                                                       well_state,
                                                       group_state,
                                                       current_step_,
                                                       guide_rate_,
                                                       well_state.wellRates(index_of_well_).data(),
                                                       phaseUsage(),
                                                       efficiencyFactor,
                                                       schedule,
                                                       summaryState,
                                                       resv_coeff,
                                                       deferred_logger);
}

template <typename FluidSystem>
bool
WellInterfaceFluidSystem<FluidSystem>::
checkGroupConstraints(WellState& well_state,
                      const GroupState& group_state,
                      const Schedule& schedule,
                      const SummaryState& summaryState,
                      DeferredLogger& deferred_logger) const
{
    const auto& well = well_ecl_;
    const int well_index = index_of_well_;

    if (well.isInjector()) {
        auto currentControl = well_state.currentInjectionControl(well_index);

        if (currentControl != Well::InjectorCMode::GRUP) {
            // This checks only the first encountered group limit,
            // in theory there could be several, and then we should
            // test all but the one currently applied. At that point,
            // this if-statement should be removed and we should always
            // check, skipping over only the single group parent whose
            // control is the active one for the well (if any).
            const auto& group = schedule.getGroup( well.groupName(), current_step_ );
            const double efficiencyFactor = well.getEfficiencyFactor();
            const std::pair<bool, double> group_constraint =
                checkGroupConstraintsInj(group, well_state, group_state, efficiencyFactor,
                                         schedule, summaryState, deferred_logger);
            // If a group constraint was broken, we set the current well control to
            // be GRUP.
            if (group_constraint.first) {
                well_state.currentInjectionControl(index_of_well_, Well::InjectorCMode::GRUP);
                const int np = well_state.numPhases();
                for (int p = 0; p<np; ++p) {
                    well_state.wellRates(index_of_well_)[p] *= group_constraint.second;
                }
            }
            return group_constraint.first;
        }
    }

    if (well.isProducer( )) {
        auto currentControl = well_state.currentProductionControl(well_index);

        if (currentControl != Well::ProducerCMode::GRUP) {
            // This checks only the first encountered group limit,
            // in theory there could be several, and then we should
            // test all but the one currently applied. At that point,
            // this if-statement should be removed and we should always
            // check, skipping over only the single group parent whose
            // control is the active one for the well (if any).
            const auto& group = schedule.getGroup( well.groupName(), current_step_ );
            const double efficiencyFactor = well.getEfficiencyFactor();
            const std::pair<bool, double> group_constraint =
                checkGroupConstraintsProd(group, well_state, group_state, efficiencyFactor,
                                          schedule, summaryState, deferred_logger);
            // If a group constraint was broken, we set the current well control to
            // be GRUP.
            if (group_constraint.first) {
                well_state.currentProductionControl(index_of_well_, Well::ProducerCMode::GRUP);
                const int np = well_state.numPhases();
                for (int p = 0; p<np; ++p) {
                    well_state.wellRates(index_of_well_)[p] *= group_constraint.second;
                }
            }
            return group_constraint.first;
        }
    }

    return false;
}

template <typename FluidSystem>
bool
WellInterfaceFluidSystem<FluidSystem>::
checkConstraints(WellState& well_state,
                 const GroupState& group_state,
                 const Schedule& schedule,
                 const SummaryState& summaryState,
                 DeferredLogger& deferred_logger) const
{
    const bool ind_broken = checkIndividualConstraints(well_state, summaryState);
    if (ind_broken) {
        return true;
    } else {
        return checkGroupConstraints(well_state, group_state, schedule, summaryState, deferred_logger);
    }
}

template<typename FluidSystem>
bool
WellInterfaceFluidSystem<FluidSystem>::
checkRateEconLimits(const WellEconProductionLimits& econ_production_limits,
                    const double* rates_or_potentials,
                    DeferredLogger& deferred_logger) const
{
    const PhaseUsage& pu = phaseUsage();

    if (econ_production_limits.onMinOilRate()) {
        assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
        const double oil_rate = rates_or_potentials[pu.phase_pos[ Oil ] ];
        const double min_oil_rate = econ_production_limits.minOilRate();
        if (std::abs(oil_rate) < min_oil_rate) {
            return true;
        }
    }

    if (econ_production_limits.onMinGasRate() ) {
        assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));
        const double gas_rate = rates_or_potentials[pu.phase_pos[ Gas ] ];
        const double min_gas_rate = econ_production_limits.minGasRate();
        if (std::abs(gas_rate) < min_gas_rate) {
            return true;
        }
    }

    if (econ_production_limits.onMinLiquidRate() ) {
        assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
        assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
        const double oil_rate = rates_or_potentials[pu.phase_pos[ Oil ] ];
        const double water_rate = rates_or_potentials[pu.phase_pos[ Water ] ];
        const double liquid_rate = oil_rate + water_rate;
        const double min_liquid_rate = econ_production_limits.minLiquidRate();
        if (std::abs(liquid_rate) < min_liquid_rate) {
            return true;
        }
    }

    if (econ_production_limits.onMinReservoirFluidRate()) {
        deferred_logger.warning("NOT_SUPPORTING_MIN_RESERVOIR_FLUID_RATE", "Minimum reservoir fluid production rate limit is not supported yet");
    }

    return false;
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
checkMaxWaterCutLimit(const WellEconProductionLimits& econ_production_limits,
                      const WellState& well_state,
                      RatioLimitCheckReport& report) const
{
    assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
    assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));

    // function to calculate water cut based on rates
    auto waterCut = [](const std::vector<double>& rates,
                       const PhaseUsage& pu) {

        const double oil_rate = rates[pu.phase_pos[Oil]];
        const double water_rate = rates[pu.phase_pos[Water]];

        // both rate should be in the same direction
        assert(oil_rate * water_rate >= 0.);

        const double liquid_rate = oil_rate + water_rate;
        if (liquid_rate != 0.) {
            return (water_rate / liquid_rate);
        } else {
            return 0.;
        }
    };

    const double max_water_cut_limit = econ_production_limits.maxWaterCut();
    assert(max_water_cut_limit > 0.);

    const bool watercut_limit_violated = checkMaxRatioLimitWell(well_state, max_water_cut_limit, waterCut);

    if (watercut_limit_violated) {
        report.ratio_limit_violated = true;
        checkMaxRatioLimitCompletions(well_state, max_water_cut_limit, waterCut, report);
    }
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
checkMaxGORLimit(const WellEconProductionLimits& econ_production_limits,
                 const WellState& well_state,
                 RatioLimitCheckReport& report) const
{
    assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
    assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));

    // function to calculate gor based on rates
    auto gor = [](const std::vector<double>& rates,
                  const PhaseUsage& pu) {

        const double oil_rate = rates[pu.phase_pos[Oil]];
        const double gas_rate = rates[pu.phase_pos[Gas]];

        // both rate should be in the same direction
        assert(oil_rate * gas_rate >= 0.);

        double gas_oil_ratio = 0.;

        if (oil_rate != 0.) {
            gas_oil_ratio = gas_rate / oil_rate;
        } else {
            if (gas_rate != 0.) {
                gas_oil_ratio = 1.e100; // big value to mark it as violated
            } else {
                gas_oil_ratio = 0.0;
            }
        }

        return gas_oil_ratio;
    };

    const double max_gor_limit = econ_production_limits.maxGasOilRatio();
    assert(max_gor_limit > 0.);

    const bool gor_limit_violated = checkMaxRatioLimitWell(well_state, max_gor_limit, gor);

    if (gor_limit_violated) {
        report.ratio_limit_violated = true;
        checkMaxRatioLimitCompletions(well_state, max_gor_limit, gor, report);
    }
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
checkMaxWGRLimit(const WellEconProductionLimits& econ_production_limits,
                 const WellState& well_state,
                 RatioLimitCheckReport& report) const
{
    assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
    assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));

    // function to calculate wgr based on rates
    auto wgr = [](const std::vector<double>& rates,
                  const PhaseUsage& pu) {

        const double water_rate = rates[pu.phase_pos[Water]];
        const double gas_rate = rates[pu.phase_pos[Gas]];

        // both rate should be in the same direction
        assert(water_rate * gas_rate >= 0.);

        double water_gas_ratio = 0.;

        if (gas_rate != 0.) {
            water_gas_ratio = water_rate / gas_rate;
        } else {
            if (water_rate != 0.) {
                water_gas_ratio = 1.e100; // big value to mark it as violated
            } else {
                water_gas_ratio = 0.0;
            }
        }

        return water_gas_ratio;
    };

    const double max_wgr_limit = econ_production_limits.maxWaterGasRatio();
    assert(max_wgr_limit > 0.);

    const bool wgr_limit_violated = checkMaxRatioLimitWell(well_state, max_wgr_limit, wgr);

    if (wgr_limit_violated) {
        report.ratio_limit_violated = true;
        checkMaxRatioLimitCompletions(well_state, max_wgr_limit, wgr, report);
    }
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
checkRatioEconLimits(const WellEconProductionLimits& econ_production_limits,
                     const WellState& well_state,
                     RatioLimitCheckReport& report,
                     DeferredLogger& deferred_logger) const
{
    // TODO: not sure how to define the worst-offending completion when more than one
    //       ratio related limit is violated.
    //       The defintion used here is that we define the violation extent based on the
    //       ratio between the value and the corresponding limit.
    //       For each violated limit, we decide the worst-offending completion separately.
    //       Among the worst-offending completions, we use the one has the biggest violation
    //       extent.

    if (econ_production_limits.onMaxWaterCut()) {
        checkMaxWaterCutLimit(econ_production_limits, well_state, report);
    }

    if (econ_production_limits.onMaxGasOilRatio()) {
        checkMaxGORLimit(econ_production_limits, well_state, report);
    }

    if (econ_production_limits.onMaxWaterGasRatio()) {
        checkMaxWGRLimit(econ_production_limits, well_state, report);
    }

    if (econ_production_limits.onMaxGasLiquidRatio()) {
        deferred_logger.warning("NOT_SUPPORTING_MAX_GLR", "the support for max Gas-Liquid ratio is not implemented yet!");
    }

    if (report.ratio_limit_violated) {
        assert(report.worst_offending_completion != INVALIDCOMPLETION);
        assert(report.violation_extent > 1.);
    }
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
updateWellTestStateEconomic(const WellState& well_state,
                            const double simulation_time,
                            const bool write_message_to_opmlog,
                            WellTestState& well_test_state,
                            DeferredLogger& deferred_logger) const
{
    if (this->wellIsStopped())
        return;

    const WellEconProductionLimits& econ_production_limits = well_ecl_.getEconLimits();

    // if no limit is effective here, then continue to the next well
    if ( !econ_production_limits.onAnyEffectiveLimit() ) {
        return;
    }

    // flag to check if the mim oil/gas rate limit is violated
    bool rate_limit_violated = false;

    const auto& quantity_limit = econ_production_limits.quantityLimit();
    const int np = number_of_phases_;
    if (econ_production_limits.onAnyRateLimit()) {
        if (quantity_limit == WellEconProductionLimits::QuantityLimit::POTN)
            rate_limit_violated = checkRateEconLimits(econ_production_limits, &well_state.wellPotentials()[index_of_well_ * np], deferred_logger);
        else {
            rate_limit_violated = checkRateEconLimits(econ_production_limits, well_state.wellRates(index_of_well_).data(), deferred_logger);
        }
    }

    if (rate_limit_violated) {
        if (econ_production_limits.endRun()) {
            const std::string warning_message = std::string("ending run after well closed due to economic limits")
                                              + std::string("is not supported yet \n")
                                              + std::string("the program will keep running after ") + name()
                                              + std::string(" is closed");
            deferred_logger.warning("NOT_SUPPORTING_ENDRUN", warning_message);
        }

        if (econ_production_limits.validFollowonWell()) {
            deferred_logger.warning("NOT_SUPPORTING_FOLLOWONWELL", "opening following on well after well closed is not supported yet");
        }

        well_test_state.closeWell(name(), WellTestConfig::Reason::ECONOMIC, simulation_time);
        if (write_message_to_opmlog) {
            if (this->well_ecl_.getAutomaticShutIn()) {
                const std::string msg = std::string("well ") + name() + std::string(" will be shut due to rate economic limit");
                deferred_logger.info(msg);
            } else {
                const std::string msg = std::string("well ") + name() + std::string(" will be stopped due to rate economic limit");
                deferred_logger.info(msg);
            }
        }
        // the well is closed, not need to check other limits
        return;
    }


    if ( !econ_production_limits.onAnyRatioLimit() ) {
        // there is no need to check the ratio limits
        return;
    }

    // checking for ratio related limits, mostly all kinds of ratio.
    RatioLimitCheckReport ratio_report;

    checkRatioEconLimits(econ_production_limits, well_state, ratio_report, deferred_logger);

    if (ratio_report.ratio_limit_violated) {
        const auto workover = econ_production_limits.workover();
        switch (workover) {
        case WellEconProductionLimits::EconWorkover::CON:
            {
                const int worst_offending_completion = ratio_report.worst_offending_completion;

                well_test_state.addClosedCompletion(name(), worst_offending_completion, simulation_time);
                if (write_message_to_opmlog) {
                    if (worst_offending_completion < 0) {
                        const std::string msg = std::string("Connection ") + std::to_string(- worst_offending_completion)
                                + std::string(" for well ") + name() + std::string(" will be closed due to economic limit");
                        deferred_logger.info(msg);
                    } else {
                        const std::string msg = std::string("Completion ") + std::to_string(worst_offending_completion)
                                + std::string(" for well ") + name() + std::string(" will be closed due to economic limit");
                        deferred_logger.info(msg);
                    }
                }

                bool allCompletionsClosed = true;
                const auto& connections = well_ecl_.getConnections();
                for (const auto& connection : connections) {
                    if (connection.state() == Connection::State::OPEN
                        && !well_test_state.hasCompletion(name(), connection.complnum())) {
                        allCompletionsClosed = false;
                    }
                }

                if (allCompletionsClosed) {
                    well_test_state.closeWell(name(), WellTestConfig::Reason::ECONOMIC, simulation_time);
                    if (write_message_to_opmlog) {
                        if (this->well_ecl_.getAutomaticShutIn()) {
                            const std::string msg = name() + std::string(" will be shut due to last completion closed");
                            deferred_logger.info(msg);
                        } else {
                            const std::string msg = name() + std::string(" will be stopped due to last completion closed");
                            deferred_logger.info(msg);
                        }
                    }
                }
                break;
            }
        case WellEconProductionLimits::EconWorkover::WELL:
            {
            well_test_state.closeWell(name(), WellTestConfig::Reason::ECONOMIC, simulation_time);
            if (write_message_to_opmlog) {
                if (well_ecl_.getAutomaticShutIn()) {
                    // tell the control that the well is closed
                    const std::string msg = name() + std::string(" will be shut due to ratio economic limit");
                    deferred_logger.info(msg);
                } else {
                    const std::string msg = name() + std::string(" will be stopped due to ratio economic limit");
                    deferred_logger.info(msg);
                }
            }
                break;
            }
        case WellEconProductionLimits::EconWorkover::NONE:
            break;
            default:
            {
                deferred_logger.warning("NOT_SUPPORTED_WORKOVER_TYPE",
                                        "not supporting workover type " + WellEconProductionLimits::EconWorkover2String(workover) );
            }
        }
    }
}

template<typename FluidSystem>
void
WellInterfaceFluidSystem<FluidSystem>::
updateWellTestState(const WellState& well_state,
                    const double& simulationTime,
                    const bool& writeMessageToOPMLog,
                    WellTestState& wellTestState,
                    DeferredLogger& deferred_logger) const
{

    // currently, we only updateWellTestState for producers
    if (this->isInjector()) {
        return;
    }

    // Based on current understanding, only under prediction mode, we need to shut well due to various
    // reasons or limits. With more knowlage or testing cases later, this might need to be corrected.
    if (!underPredictionMode() ) {
        return;
    }

    // updating well test state based on physical (THP/BHP) limits.
    updateWellTestStatePhysical(well_state, simulationTime, writeMessageToOPMLog, wellTestState, deferred_logger);

    // updating well test state based on Economic limits.
    updateWellTestStateEconomic(well_state, simulationTime, writeMessageToOPMLog, wellTestState, deferred_logger);

    // TODO: well can be shut/closed due to other reasons
}

template<typename FluidSystem>
template <typename RatioFunc>
void WellInterfaceFluidSystem<FluidSystem>::
checkMaxRatioLimitCompletions(const WellState& well_state,
                              const double max_ratio_limit,
                              const RatioFunc& ratioFunc,
                              RatioLimitCheckReport& report) const
{
    int worst_offending_completion = INVALIDCOMPLETION;

    // the maximum water cut value of the completions
    // it is used to identify the most offending completion
    double max_ratio_completion = 0;
    const int np = number_of_phases_;

    const auto * perf_phase_rates = &well_state.perfPhaseRates()[first_perf_ * np];
    // look for the worst_offending_completion
    for (const auto& completion : completions_) {
        std::vector<double> completion_rates(np, 0.0);

        // looping through the connections associated with the completion
        const std::vector<int>& conns = completion.second;
        for (const int c : conns) {
            for (int p = 0; p < np; ++p) {
                const double connection_rate = perf_phase_rates[c * np + p];
                completion_rates[p] += connection_rate;
            }
        } // end of for (const int c : conns)

        parallel_well_info_.communication().sum(completion_rates.data(), completion_rates.size());
        const double ratio_completion = ratioFunc(completion_rates, phaseUsage());

        if (ratio_completion > max_ratio_completion) {
            worst_offending_completion = completion.first;
            max_ratio_completion = ratio_completion;
        }
    } // end of for (const auto& completion : completions_)

    assert(max_ratio_completion > max_ratio_limit);
    assert(worst_offending_completion != INVALIDCOMPLETION);
    const double violation_extent = max_ratio_completion / max_ratio_limit;
    assert(violation_extent > 1.0);

    if (violation_extent > report.violation_extent) {
        report.worst_offending_completion = worst_offending_completion;
        report.violation_extent = violation_extent;
    }
}

template<typename FluidSystem>
template<typename RatioFunc>
bool WellInterfaceFluidSystem<FluidSystem>::
checkMaxRatioLimitWell(const WellState& well_state,
                       const double max_ratio_limit,
                       const RatioFunc& ratioFunc) const
{
    const int np = number_of_phases_;

    std::vector<double> well_rates(np, 0.0);

    for (int p = 0; p < np; ++p) {
        well_rates[p] = well_state.wellRates(index_of_well_)[p];
    }

    const double well_ratio = ratioFunc(well_rates, phaseUsage());

    return (well_ratio > max_ratio_limit);
}

template class WellInterfaceFluidSystem<BlackOilFluidSystem<double,BlackOilDefaultIndexTraits>>;
template class WellInterfaceFluidSystem<BlackOilFluidSystem<double,EclAlternativeBlackOilIndexTraits>>;

} // namespace Opm
