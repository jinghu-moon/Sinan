import { createRouter, createWebHashHistory } from 'vue-router'
import HomeView from '../views/HomeView.vue'

const router = createRouter({
  history: createWebHashHistory(import.meta.env.BASE_URL),
  routes: [
    { path: '/', component: HomeView },
    { path: '/appearance', component: () => import('../views/AppearanceView.vue') },
    { path: '/rules', component: () => import('../views/RulesView.vue') },
    { path: '/directories', component: () => import('../views/DirectoriesView.vue') },
    { path: '/shortcuts', component: () => import('../views/ShortcutsView.vue') },
    { path: '/tabs', component: () => import('../views/TabsView.vue') },
  ],
})

export default router
