FROM golang:1.26-alpine AS builder

WORKDIR /app

COPY go.mod go.sum ./
RUN go mod download

COPY schema/ ./schema/
COPY backend/ ./backend/

RUN go build -o /app/backend-bin ./backend/main.go

FROM alpine:latest

WORKDIR /app

COPY --from=builder /app/backend-bin ./backend

CMD ["./backend"]