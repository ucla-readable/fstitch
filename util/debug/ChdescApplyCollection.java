import java.io.DataInput;
import java.io.IOException;

public class ChdescApplyCollection extends Opcode
{
	private final int count, chdescs, order;
	
	public ChdescApplyCollection(int count, int chdescs, int order)
	{
		this.count = count;
		this.chdescs = chdescs;
		this.order = order;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_APPLY_COLLECTION, "KDB_CHDESC_APPLY_COLLECTION", ChdescApplyCollection.class);
		factory.addParameter("count", 4);
		factory.addParameter("chdescs", 4);
		factory.addParameter("order", 4);
		return factory;
	}
}
