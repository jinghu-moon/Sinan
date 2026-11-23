import { describe, it, expect } from 'vitest'

import { mount } from '@vue/test-utils'
import App from '../App.vue'
import router from '../router'

describe('App', () => {
  it('renders the settings home and its Tabler icon', async () => {
    await router.push('/')
    const wrapper = mount(App, { global: { plugins: [router] } })
    expect(wrapper.get('main h1').text()).toBe('常规')
    expect(wrapper.get('nav svg').attributes('aria-hidden')).toBe('true')
    expect(wrapper.get('[role="switch"][aria-label="登录自启"]').attributes('aria-disabled')).toBe(
      'true',
    )
  })
})
