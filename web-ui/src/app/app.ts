import { Component, OnInit, inject } from '@angular/core';
import { ApiService } from './core/api.service';
import { DashboardComponent } from './dashboard/dashboard';
import { InjectComponent } from './inject/inject';
import { InterceptComponent } from './intercept/intercept';
import { StreamComponent } from './stream/stream';
import { Tab, Workbench } from './core/workbench.service';

@Component({
  imports: [StreamComponent, DashboardComponent, InterceptComponent, InjectComponent],
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
    { id: 'dashboard', label: 'Dashboard' },
  ];

  ngOnInit(): void {
    this.api.connect();
  }
}
