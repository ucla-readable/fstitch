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

		public Factory(GrouperFactory subGrouperFactory, String color)
		{
			super(subGrouperFactory);
			this.color = color;

			name = "block[" + color + "]";
			String subName = subGrouperFactory.toString();
			if (!subName.equals(NoneGrouper.Factory.getFactory().toString()))
				name += "-" + subName;
		}

		public Grouper newInstance()
		{
			return new BlockGrouper(subGrouperFactory, color);
		}

		public String toString()
		{
			return name;
		}

	}


	protected String color;

	public BlockGrouper(GrouperFactory subGrouperFactory, String color)
	{
		super(subGrouperFactory);
		this.color = color;
	}

	protected Object getGroupKey(Chdesc c)
	{
		return new Integer(c.getBlock());
	}

	protected void renderGroup(Object groupKey, Grouper subGrouper, String clusterPrefix, Writer output) throws IOException
	{
		Integer block = (Integer) groupKey;
		String clusterName = clusterPrefix + block;
		if(block.intValue() != 0)
		{
			output.write("subgraph cluster" + clusterName + " {\n");
			output.write("label=\"block " + SystemState.hex(block.intValue())
			             + "\";\n");
			output.write("color=" + color + ";\n");
			output.write("labeljust=r;\n");
		}
		subGrouper.render(clusterName, output);
		if(block.intValue() != 0)
			output.write("}\n");
	}
}
