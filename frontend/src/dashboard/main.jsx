import { render } from 'preact';
import { App } from './app.jsx';
import '../styles.css';
import '../components/history.css';
import './changes.css';

render(<App />, document.getElementById('app'));
