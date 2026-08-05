/**
  * @file    iown_node_types.h
  * @author  iown-homecontrol
  * @brief   Actuator node type identifiers
  *
  * The type codes an actuator reports in its node descriptor. Sub-types are
  * listed underneath as reverse-engineering notes; they use a "major-minor"
  * notation that is not valid C, so they stay in comments until someone
  * confirms how the two halves are actually encoded on the wire.
  */

#pragma once
/* Define to prevent recursive inclusion */

/*
 * The enumerators are shared between C and C++.
 *
 * They were written as a C++ `enum class` inside an `extern "C"` block, which
 * meant this header could not be compiled as C at all - the linkage block
 * suggested otherwise, but `enum class` is a syntax error there. C gets a
 * plain enum with prefixed names; C++ keeps the scoped enum. Both carry the
 * same values, so a code read off the wire means the same thing either way.
 */

#ifdef __cplusplus

enum class iown_node_actuator_types {
  Unknown = 0,
  VenetianBlind = 1,
  RollingShutter = 2,
  VerticalAwning = 3,
  WindowOpener = 4,
  GarageDoorOpener = 5,
  Light = 6,
  GateOpener = 7,
  RollingDoorOpener = 8,
  MotorizedBolt = 9,
  InteriorBlind = 10,
  SCD = 11,
  Beacon = 12,
  DualShutter = 13,
  TemperatureControlInterface = 14,
  OnOffSwitch = 15,
  HorizontalAwning = 16,
  ExternalVenetianBlind = 17,
  LouvreBlind = 18,
  CurtainTrack = 19,
  VentilationPoint = 20,
  ExteriorHeating = 21,
  HeatPump = 22,
  IntrusionAlarm = 23,
  SwingingShutter = 24,
};

#else

enum iown_node_actuator_types {
  IOWN_NODE_UNKNOWN = 0,
  IOWN_NODE_VENETIAN_BLIND = 1,
  IOWN_NODE_ROLLING_SHUTTER = 2,
  IOWN_NODE_VERTICAL_AWNING = 3,
  IOWN_NODE_WINDOW_OPENER = 4,
  IOWN_NODE_GARAGE_DOOR_OPENER = 5,
  IOWN_NODE_LIGHT = 6,
  IOWN_NODE_GATE_OPENER = 7,
  IOWN_NODE_ROLLING_DOOR_OPENER = 8,
  IOWN_NODE_MOTORIZED_BOLT = 9,
  IOWN_NODE_INTERIOR_BLIND = 10,
  IOWN_NODE_SCD = 11,
  IOWN_NODE_BEACON = 12,
  IOWN_NODE_DUAL_SHUTTER = 13,
  IOWN_NODE_TEMPERATURE_CONTROL_INTERFACE = 14,
  IOWN_NODE_ON_OFF_SWITCH = 15,
  IOWN_NODE_HORIZONTAL_AWNING = 16,
  IOWN_NODE_EXTERNAL_VENETIAN_BLIND = 17,
  IOWN_NODE_LOUVRE_BLIND = 18,
  IOWN_NODE_CURTAIN_TRACK = 19,
  IOWN_NODE_VENTILATION_POINT = 20,
  IOWN_NODE_EXTERIOR_HEATING = 21,
  IOWN_NODE_HEAT_PUMP = 22,
  IOWN_NODE_INTRUSION_ALARM = 23,
  IOWN_NODE_SWINGING_SHUTTER = 24,
};

#endif /* __cplusplus */

/*
 * Sub-types, as "<type>-<variant>" or "<type>.<variant>". Reverse-engineering
 * notes only - the notation below is not valid C in either dialect, and
 * several names repeat, so this cannot become an enum as written.
 *
 *    1.1     Blind                 (VenetianBlind)
 *    1.2     Slats                 (VenetianBlind)
 *    2-1     RollerShutter         (RollingShutter)
 *    2-1.1   Blind                 (RollingShutter)
 *    2-1.2   Slats                 (RollingShutter)
 *    2-2     RollerShutter         (RollingShutter)
 *    4-1     WindowOperator        (WindowOpener)
 *    5-58    GarageOpener          (GarageDoorOpener)
 *    6-58    Light                 (Light)
 *    7-58    GateOpener            (GateOpener)
 *    9-1     Lock                  (MotorizedBolt)
 *   13.1     BothCurtains          (DualShutter)
 *   13.2     UpperCurtain          (DualShutter)
 *   13.3     LowerCurtain          (DualShutter)
 *   17.1     Blind                 (ExternalVenetianBlind)
 *   17.2     Slats                 (ExternalVenetianBlind)
 *   18.1     Curtain               (LouvreBlind)
 *   18.2     Hangers               (LouvreBlind)
 *   20-1     VentilationPoint      (VentilationPoint)
 *   20-2     VentilationPoint      (VentilationPoint)
 *   20-3     VentilationPoint      (VentilationPoint)
 *   21-58    OutdoorHeating        (ExteriorHeating)
 *   24-1     SwingingShutter       (SwingingShutter)
 */
