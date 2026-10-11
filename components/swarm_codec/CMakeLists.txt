# The pure swarm wire codec (swarm_frame.{c,h}): no ESP-IDF, no WiFi. Split
# out of the swarm component so the H2 coordinator's uart_link can reuse the
# frame encode/decode without pulling swarm's WiFi/ESP-NOW sources (which do
# not build on the radio-less H2). Host-tested directly with plain cc.
idf_component_register(SRCS "swarm_frame.c" INCLUDE_DIRS "include")
