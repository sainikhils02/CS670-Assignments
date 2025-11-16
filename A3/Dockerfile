FROM ubuntu:22.04

RUN apt-get update && \
    apt-get install -y build-essential libboost-all-dev libssl-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN g++ -std=c++20 -pthread server.cpp -o p0 -DROLE_p0 -lboost_system -lssl -lcrypto && \
    g++ -std=c++20 -pthread server.cpp -o p1 -DROLE_p1 -lboost_system -lssl -lcrypto && \
    g++ -std=c++20 -pthread p2.cpp -o p2 -lboost_system && \
    g++ -std=c++20 -pthread gen_queries.cpp -o gen_queries -lboost_system -lssl -lcrypto

CMD ["/bin/bash"]
