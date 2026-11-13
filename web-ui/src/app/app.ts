import { Component, OnInit, inject } from '@angular/core';
import { ApiService } from './core/api.service';
import { DashboardComponent } from './dashboard/dashboard';
import { InjectComponent } from './inject/inject';
import { InterceptComponent } from './intercept/intercept';
import { PluginsComponent } from './plugins/plugins';
import { SettingsComponent } from './settings/settings';
import { WorldsComponent } from './worlds/worlds';
import { StreamComponent } from './stream/stream';
import { Tab, Workbench } from './core/workbench.service';

@Component({
  imports: [StreamComponent, DashboardComponent, InterceptComponent, InjectComponent, PluginsComponent, SettingsComponent, WorldsComponent],
  selector: 'app-root',
  styleUrl: './app.css',
  templateUrl: './app.html',
})
export class App implements OnInit {
  protected readonly api = inject(ApiService);
  protected readonly bench = inject(Workbench);
  protected readonly tab = this.bench.tab;
  protected readonly tabs: { id: Tab; label: string }[] = [
    { id: 'stream', label: 'Packets' },
    { id: 'intercept', label: 'Intercept' },
    { id: 'inject', label: 'Inject' },
    { id: 'worlds', label: 'Worlds' },
    { id: 'plugins', label: 'Plugins' },
    { id: 'dashboard', label: 'Dashboard' },
    { id: 'settings', label: 'Profiles & sessions' },
  ];

  ngOnInit(): void {
    this.api.connect();
  }
}
