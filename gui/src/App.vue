<script setup lang="ts">
import {
  IconAlertCircle,
  IconAlertTriangle,
  IconCircleCheck,
  IconFolderOpen,
  IconFolders,
  IconKeyboard,
  IconLayoutNavbar,
  IconLayout,
  IconSettings,
  IconX,
} from '@tabler/icons-vue'
import { Toaster } from 'vue-sonner'
import { RouterLink, RouterView } from 'vue-router'
import 'vue-sonner/style.css'
import 'tdesign-vue-next/es/style/index.css'

const brandIcon = `${import.meta.env.BASE_URL}favicon.ico`
const notificationIcons = {
  success: IconCircleCheck,
  warning: IconAlertTriangle,
  error: IconAlertCircle,
  close: IconX,
}
</script>

<template>
  <div class="window">
    <aside class="sidebar">
      <div class="brand">
        <img :src="brandIcon" alt="" width="40" height="40" />
        <span>司南</span>
      </div>
      <nav aria-label="设置导航">
        <RouterLink to="/" class="nav-entry">
          <IconSettings :size="20" stroke="1.7" aria-hidden="true" />
          常规
        </RouterLink>
        <RouterLink to="/appearance" class="nav-entry">
          <IconLayoutNavbar :size="20" stroke="1.7" aria-hidden="true" />
          标签外观
        </RouterLink>
        <RouterLink to="/rules" class="nav-entry">
          <IconFolders :size="20" stroke="1.7" aria-hidden="true" />
          目录规则
        </RouterLink>
        <RouterLink to="/directories" class="nav-entry">
          <IconFolderOpen :size="20" stroke="1.7" aria-hidden="true" />
          目录入口
        </RouterLink>
        <RouterLink to="/shortcuts" class="nav-entry">
          <IconKeyboard :size="20" stroke="1.7" aria-hidden="true" />
          快捷键
        </RouterLink>
        <RouterLink to="/tabs" class="nav-entry">
          <IconLayout :size="20" stroke="1.7" aria-hidden="true" />
          标签管理
        </RouterLink>
      </nav>
    </aside>
    <RouterView />
  </div>
  <Toaster
    position="bottom-right"
    container-aria-label="司南通知"
    rich-colors
    close-button
    :icons="notificationIcons"
    :toast-options="{ closeButtonAriaLabel: '关闭通知' }"
  />
</template>

<style>
:root {
  color-scheme: light;
  --ink: #18272a;
  --muted: #526367;
  --line: #e4ebea;
  --accent: #137565;
  --warning: #8d4c15;
  --t-brand-color: #137565;
  --t-brand-color-hover: #196e60;
  --t-brand-color-active: #0e6052;
  font:
    14px/1.5 'Segoe UI',
    'Microsoft YaHei UI',
    sans-serif;
  color: var(--ink);
  background: #f3f6f5;
}

body {
  margin: 0;
}

.content {
  max-width: 1080px;
  margin: 0 auto;
  padding: 44px clamp(24px, 4vw, 68px);
}

.card {
  border: 1px solid #dde7e4;
  border-radius: 14px;
  background: #fff;
  box-shadow: 0 2px 5px #253a3408;
}

@media (max-width: 540px) {
  .content {
    padding: 24px 16px;
  }
}
</style>

<style scoped>
.window {
  display: grid;
  grid-template-columns: 208px minmax(0, 1fr);
  min-height: 100dvh;
}

.sidebar {
  padding: 30px 16px;
  background: #e9f0ee;
  border-right: 1px solid #dce6e3;
}

.brand {
  display: flex;
  align-items: center;
  gap: 12px;
  margin: 0 9px 30px;
  font-size: 24px;
  font-weight: 650;
}

nav {
  display: grid;
  gap: 4px;
}

.nav-entry {
  display: flex;
  align-items: center;
  gap: 11px;
  padding: 10px 13px;
  border-radius: 8px;
  color: var(--muted);
  font-weight: 650;
  text-decoration: none;
  white-space: nowrap;
}

.nav-entry:hover {
  background: #dfece8;
}

.nav-entry.router-link-exact-active {
  background: #d6e9e3;
  color: #134d43;
}

@media (max-width: 880px) {
  .window {
    grid-template-columns: 150px minmax(0, 1fr);
  }
}

@media (max-width: 540px) {
  .window {
    grid-template-columns: minmax(0, 1fr);
    grid-template-rows: auto 1fr;
  }

  .sidebar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 12px 16px;
    border-right: 0;
    border-bottom: 1px solid #dce6e3;
  }

  .brand {
    margin: 0;
  }

  nav {
    display: flex;
    flex-wrap: wrap;
  }
}
</style>
