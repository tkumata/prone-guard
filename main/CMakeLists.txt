idf_component_register(
    SRCS "main.c" "prone_inference_bridge.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "models/prone.espdl"
)
