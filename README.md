DeFCoN integration/staging tree
===============================

Copyright (c) 2025 DeFCoN Developers


How do I build the software?
----------------------------

The easiest way to build the repository is via the depends method:

    apt update
    apt install -y build-essential automake autotools-dev cmake pkg-config python3 bison git make libtool
    git clone https://github.com/defcon-project/defcon
    cd defcon/depends
    make HOST=x86_64-pc-linux-gnu -j4
    cd ..
    ./autogen.sh
    CONFIG_SITE=$PWD/depends/x86_64-pc-linux-gnu/share/config.site ./configure
    make -j4



