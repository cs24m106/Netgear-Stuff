FROM gcc AS builder

WORKDIR /app

COPY s_server.c .

RUN gcc s_server.c -o server

FROM ubuntu:latest

WORKDIR /app

COPY --from=builder /app/server .

EXPOSE 8080

CMD ["./server", " 8080"]
