import DefaultTheme from 'vitepress/theme';
import './custom.css';

// The default theme, restyled through CSS variables only. Nothing here
// overrides a component: the energese tokens are applied by remapping
// VitePress's own variables in custom.css, so a VitePress upgrade cannot break
// a component override we never wrote.
export default DefaultTheme;
