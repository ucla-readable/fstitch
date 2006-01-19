import java.io.IOException;
import java.io.Writer;

/**
 * A Grouper that groups a Chdesc by the block it is on.
 */
public class BlockGrouper extends AbstractGrouper
{
	public static class Factory extends AbstractGrouper.Factory
	{
		protected String color;
		protected String name;

			public Factory(GrouperFactory subGrouperFactory, String color, Debugger dbg)
		{
			super(subGrouperFactory, dbg);
			this.color = color;

			name = "block[" + color + "]";
			String subName = subGrouperFactory.toString();
			if (!subName.equals(NoneGrouper.Factory.getFactory().toString()))
				name += "-" + subName;
		}

		public Grouper newInstance()
		{
			return new BlockGrouper(subGrouperFactory, color, debugger);
		}

		public String toString()
		{
			return name;
		}

	}


	protected String color;

	public BlockGrouper(GrouperFactory subGrouperFactory, String color, Debugger dbg)
	{
		super(subGrouperFactory, dbg);
		this.color = color;
	}

	protected Object getGroupKey(Chdesc c)
	{
		return new Integer(c.getBlock());
	}

	protected void renderGroup(Object groupKey, Grouper subGrouper, String clusterPrefix, Writer output) throws IOException
	{
		int block = ((Integer) groupKey).intValue();
		String clusterName = clusterPrefix + block;
		if(block != 0)
		{
			output.write("subgraph cluster" + clusterName + " {\n");

			String blockName = Chdesc.getBlockName(block, 0, true, false, debugger.getState());
			output.write("label=\"" + blockName + "\";\n");

			output.write("color=" + color + ";\n");
			output.write("labeljust=r;\n");
		}
		subGrouper.render(clusterName, output);
		if(block != 0)
			output.write("}\n");
	}
}
