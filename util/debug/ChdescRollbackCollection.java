import java.io.DataInput;
//import java.io.IOException;

public class ChdescRollbackCollection extends Opcode
{
	private final int count, chdescs, order;
	
	public ChdescRollbackCollection(int count, int chdescs, int order)
	{
		this.count = count;
		this.chdescs = chdescs;
		this.order = order;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_ROLLBACK_COLLECTION: count = " + count + ", chdescs = " + SystemState.hex(chdescs) + ", order = " + SystemState.hex(order);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ROLLBACK_COLLECTION, "KDB_CHDESC_ROLLBACK_COLLECTION", ChdescRollbackCollection.class);
		factory.addParameter("count", 4);
		factory.addParameter("chdescs", 4);
		factory.addParameter("order", 4);
		return factory;
	}
}
