#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
from pandas.plotting import table

def generate_table():
    # Read the CSV
    csv_path = 'statistics/benchmark_heuristic_results.csv'
    output_path = 'statistics/benchmark_heuristic_table.png'
    
    try:
        df = pd.read_csv(csv_path)
    except FileNotFoundError:
        print(f"Error: Could not find {csv_path}")
        return

    # Select relevant columns for the table to keep it readable
    # Renaming for better display
    display_columns = {
        'test_case': 'Test Case',
        'num_teachers': 'Teachers',
        'num_classrooms': 'Rooms',
        'num_sections': 'Sections',
        'heuristic_total_ms': 'Time (ms)',
        'heuristic_objective': 'Objective'
    }
    
    # Filter and rename
    df_display = df[list(display_columns.keys())].rename(columns=display_columns)

    # Calculate figure size based on rows and columns
    # Approx 1.2 inch height per row for header + rows, width auto adjusted
    fig_width = 12
    fig_height = min(len(df) * 0.5 + 1, 10) 
    
    fig, ax = plt.subplots(figsize=(fig_width, fig_height))
    
    # Hide axes
    ax.xaxis.set_visible(False)
    ax.yaxis.set_visible(False)
    ax.set_frame_on(False)
    
    # Create the table
    # transform df to values for cellText, colLabels for header
    cell_text = []
    for row in df_display.values:
        cell_text.append(row)
        
    tab = ax.table(cellText=cell_text, colLabels=df_display.columns, loc='center', cellLoc='center')
    
    # Style the table
    tab.auto_set_font_size(False)
    tab.set_fontsize(10)
    tab.scale(1.2, 1.5)  # Scale width, height

    # Styling specific cells (Header)
    for k, cell in tab.get_celld().items():
        row, col = k
        if row == 0:
            cell.set_text_props(weight='bold', color='white')
            cell.set_facecolor('#40466e')
        else:
            # Alternating row colors
            if row % 2 == 0:
                cell.set_facecolor('#f1f1f2')
            else:
                cell.set_facecolor('white')
                
    plt.title('Heuristic Benchmark Results (Test Cases 11-20)', pad=20, fontsize=14, weight='bold')
    
    # Save the figure
    plt.savefig(output_path, bbox_inches='tight', dpi=300)
    print(f"Table generated successfully: {output_path}")

if __name__ == "__main__":
    generate_table()
