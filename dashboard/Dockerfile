FROM node:20-alpine

WORKDIR /app

COPY dashboard/package.json ./package.json
RUN npm install

COPY proto ./proto
COPY dashboard/server.js ./server.js
COPY dashboard/public ./public

EXPOSE 8080
CMD ["npm", "start"]
