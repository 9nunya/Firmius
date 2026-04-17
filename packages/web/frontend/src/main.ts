import { renderPlaceholderApp } from "./app";

const target = document.getElementById("app");

if (target) {
  target.innerHTML = renderPlaceholderApp();
}
