module.exports = {
    dependency: {
        platforms: {
            kepler: {
                "autolink": {
                    "ProbeNative": {
                        "libraryName": "libProbeNative.so",
                        "linkDynamic": true,
                        "provider": "application",
                        "components": [],
                        "turbomodules": [
                            "ProbeNative"
                        ]
                    }
                }
            },
        },
    },
};